//! Host-only registry. No JNI types, C atomics, TLS, libc selection or locks.
//! All allocations go through the owning C runtime's callbacks.
const std = @import("std");
const Allocator = std.mem.Allocator;
const Alignment = std.mem.Alignment;
const block_slots = 64;
const ok = 0;
const memory_error = 1;
const argument_error = 2;
const busy = 4;

pub const panic = std.debug.FullPanic(struct {
    fn call(message: []const u8, address: ?usize) noreturn {
        _ = message;
        _ = address;
        @trap();
    }
}.call);

const Callbacks = extern struct {
    object_size: usize,
    object_alignment: usize,
    signature_offset: usize,
    signature_capacity: usize,
    context: ?*anyopaque,
    allocate: ?*const fn (?*anyopaque, usize, usize) callconv(.c) ?*anyopaque,
    deallocate: ?*const fn (?*anyopaque, *anyopaque) callconv(.c) void,
    prepare: ?*const fn (?*anyopaque, *anyopaque, u64, usize, [*:0]const u8) callconv(.c) c_int,
    retire: ?*const fn (?*anyopaque, *anyopaque) callconv(.c) c_int,
};

const Key = struct { guest: usize, signature: []const u8 };
const KeyContext = struct {
    pub fn hash(_: @This(), key: Key) u64 {
        return std.hash.Wyhash.hash(@intCast(key.guest), key.signature);
    }

    pub fn eql(_: @This(), a: Key, b: Key) bool {
        return a.guest == b.guest and std.mem.eql(u8, a.signature, b.signature);
    }
};

const Chunk = struct {
    storage: [*]u8,
    occupied: u64 = 0,
    guests: [block_slots]usize = @splat(0),
};
const Location = struct { chunk: *Chunk, index: u6 };
const Index = std.HashMapUnmanaged(Key, Location, KeyContext, 80);

const Module = struct {
    chunks: std.ArrayList(*Chunk) = .empty,
    available: std.ArrayList(*Chunk) = .empty,
    index: Index = .empty,

    fn deinit(self: *Module, store: *Store) void {
        const allocator = store.allocator();
        for (self.chunks.items) |chunk| {
            store.callbacks.deallocate.?(store.callbacks.context, chunk.storage);
            allocator.destroy(chunk);
        }
        self.chunks.deinit(allocator);
        self.available.deinit(allocator);
        self.index.deinit(allocator);
        allocator.destroy(self);
    }

    fn vacant(self: *Module, store: *Store) !Location {
        if (self.available.items.len == 0) {
            const allocator = store.allocator();
            const count = self.chunks.items.len + 1;
            try self.chunks.ensureTotalCapacity(allocator, count);
            // One available-list position per chunk also makes discard infallible.
            try self.available.ensureTotalCapacity(allocator, count);
            const chunk = try allocator.create(Chunk);
            errdefer allocator.destroy(chunk);
            const bytes = std.math.mul(usize, store.callbacks.object_size, block_slots) catch
                return error.OutOfMemory;
            const raw = store.callbacks.allocate.?(
                store.callbacks.context,
                bytes,
                store.callbacks.object_alignment,
            ) orelse return error.OutOfMemory;
            chunk.* = .{ .storage = @ptrCast(raw) };
            self.chunks.appendAssumeCapacity(chunk);
            self.available.appendAssumeCapacity(chunk);
        }
        const chunk = self.available.items[self.available.items.len - 1];
        return .{ .chunk = chunk, .index = @intCast(@ctz(~chunk.occupied)) };
    }

    fn insert(self: *Module, store: *Store, module: u64, key: Key, status: *c_int) !Location {
        try self.index.ensureUnusedCapacity(store.allocator(), 1);
        const slot = try self.vacant(store);
        const object = store.object(slot);
        @memset(object, 0);
        status.* = store.callbacks.prepare.?(
            store.callbacks.context,
            object.ptr,
            module,
            key.guest,
            @ptrCast(key.signature.ptr),
        );
        if (status.* != ok) return error.PrepareFailed;

        // The C object owns the descriptor; both it and the CIF remain in place.
        const stable_key = Key{ .guest = key.guest, .signature = store.signature(slot, key.signature.len) };
        self.index.putAssumeCapacityNoClobber(stable_key, slot);
        slot.chunk.guests[slot.index] = key.guest;
        slot.chunk.occupied |= @as(u64, 1) << slot.index;
        if (slot.chunk.occupied == std.math.maxInt(u64)) _ = self.available.pop();
        return slot;
    }

    fn erase(self: *Module, store: *Store, slot: Location, length: usize) c_int {
        if (store.callbacks.retire.?(store.callbacks.context, store.object(slot).ptr) == 0) return busy;
        const key = Key{ .guest = slot.chunk.guests[slot.index], .signature = store.signature(slot, length) };
        std.debug.assert(self.index.remove(key));
        if (slot.chunk.occupied == std.math.maxInt(u64)) self.available.appendAssumeCapacity(slot.chunk);
        slot.chunk.occupied &= ~(@as(u64, 1) << slot.index);
        @memset(store.object(slot), 0);
        return ok;
    }
};

const Store = struct {
    callbacks: Callbacks,
    modules: std.AutoHashMapUnmanaged(u64, *Module) = .empty,

    fn allocator(self: *Store) Allocator {
        return .{ .ptr = self, .vtable = &.{
            .alloc = hostAlloc,
            .resize = Allocator.noResize,
            .remap = Allocator.noRemap,
            .free = hostFree,
        } };
    }

    fn object(self: *Store, slot: Location) []u8 {
        const size = self.callbacks.object_size;
        return (slot.chunk.storage + @as(usize, slot.index) * size)[0..size];
    }

    fn signature(self: *Store, slot: Location, length: usize) []const u8 {
        const offset = self.callbacks.signature_offset;
        return self.object(slot)[offset..][0..length];
    }

    fn moduleFor(self: *Store, id: u64) !*Module {
        if (self.modules.get(id)) |module| return module;
        const allocator_ = self.allocator();
        const module = try allocator_.create(Module);
        errdefer allocator_.destroy(module);
        module.* = .{};
        try self.modules.put(allocator_, id, module);
        return module;
    }

    fn dropEmpty(self: *Store, id: u64, module: *Module) void {
        if (module.index.count() != 0) return;
        std.debug.assert(self.modules.remove(id));
        module.deinit(self);
    }
};

fn hostAlloc(context: *anyopaque, size: usize, alignment: Alignment, _: usize) ?[*]u8 {
    const store: *Store = @ptrCast(@alignCast(context));
    const memory = store.callbacks.allocate.?(store.callbacks.context, size, alignment.toByteUnits()) orelse return null;
    return @ptrCast(memory);
}

fn hostFree(context: *anyopaque, memory: []u8, _: Alignment, _: usize) void {
    const store: *Store = @ptrCast(@alignCast(context));
    store.callbacks.deallocate.?(store.callbacks.context, memory.ptr);
}

fn create(callbacks: ?*const Callbacks) callconv(.c) ?*Store {
    const c = (callbacks orelse return null).*;
    if (c.allocate == null or c.deallocate == null or c.prepare == null or c.retire == null or
        c.object_size == 0 or !std.math.isPowerOfTwo(c.object_alignment) or
        c.object_size % c.object_alignment != 0 or c.signature_capacity < 2 or
        c.signature_offset > c.object_size or c.signature_capacity > c.object_size - c.signature_offset)
        return null;
    const memory = c.allocate.?(c.context, @sizeOf(Store), @alignOf(Store)) orelse return null;
    const store: *Store = @ptrCast(@alignCast(memory));
    store.* = .{ .callbacks = c };
    return store;
}

fn get(store_: ?*Store, module_id: u64, guest: usize, text: [*:0]const u8, length: usize, object_: *?*anyopaque, created: *c_int) callconv(.c) c_int {
    object_.* = null;
    created.* = 0;
    const store = store_ orelse return argument_error;
    if (module_id == 0 or guest == 0 or length == 0 or length >= store.callbacks.signature_capacity or
        text[length] != 0 or std.mem.findScalar(u8, text[0..length], 0) != null)
        return argument_error;
    const module = store.moduleFor(module_id) catch return memory_error;
    defer store.dropEmpty(module_id, module);
    const key = Key{ .guest = guest, .signature = text[0..length] };
    if (module.index.get(key)) |slot| {
        object_.* = store.object(slot).ptr;
        return ok;
    }
    var status: c_int = ok;
    const slot = module.insert(store, module_id, key, &status) catch |err| return switch (err) {
        error.OutOfMemory => memory_error,
        error.PrepareFailed => status,
    };
    object_.* = store.object(slot).ptr;
    created.* = 1;
    return ok;
}

fn discard(store_: ?*Store, module_id: u64, guest: usize, text: [*:0]const u8, length: usize) callconv(.c) c_int {
    const store = store_ orelse return argument_error;
    const module = store.modules.get(module_id) orelse return argument_error;
    const slot = module.index.get(.{ .guest = guest, .signature = text[0..length] }) orelse return argument_error;
    const result = module.erase(store, slot, length);
    store.dropEmpty(module_id, module);
    return result;
}

fn release(store_: ?*Store, module_id: u64) callconv(.c) c_int {
    const store = store_ orelse return argument_error;
    const module = store.modules.get(module_id) orelse return ok;
    var result: c_int = ok;
    for (module.chunks.items) |chunk| {
        var pending = chunk.occupied;
        while (pending != 0) {
            const index: u6 = @intCast(@ctz(pending));
            pending &= pending - 1;
            const slot = Location{ .chunk = chunk, .index = index };
            const signature = store.object(slot)[store.callbacks.signature_offset..][0..store.callbacks.signature_capacity];
            const length = std.mem.findScalar(u8, signature, 0) orelse unreachable;
            if (module.erase(store, slot, length) != ok) result = busy;
        }
    }
    store.dropEmpty(module_id, module);
    return result;
}

fn destroy(store_: ?*Store) callconv(.c) c_int {
    const store = store_ orelse return ok;
    // An empty registry is the only legal destruction state. The caller must
    // retire modules under the same unload/quiescence contract as the C backend.
    if (store.modules.count() != 0) return busy;
    const callbacks = store.callbacks;
    store.modules.deinit(store.allocator());
    callbacks.deallocate.?(callbacks.context, store);
    return ok;
}

comptime {
    @export(&create, .{ .name = "gw_binding_store_create", .visibility = .hidden });
    @export(&get, .{ .name = "gw_binding_store_get", .visibility = .hidden });
    @export(&discard, .{ .name = "gw_binding_store_discard", .visibility = .hidden });
    @export(&release, .{ .name = "gw_binding_store_release", .visibility = .hidden });
    @export(&destroy, .{ .name = "gw_binding_store_destroy", .visibility = .hidden });
}
