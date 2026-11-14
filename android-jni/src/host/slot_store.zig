//! Stable C-owned objects, generational tokens, dense owner membership.
//! No JNI, libc, locks or per-object allocation. Caller supplies serialization.
const std = @import("std");
const Allocator = std.mem.Allocator;
const Alignment = std.mem.Alignment;
const width = 64;
const Config = extern struct {
    object_size: usize,
    object_alignment: usize,
    owner_count: usize,
    control_limit: usize,
    generation_max: u32,
    context: ?*anyopaque,
    allocate: ?*const fn (?*anyopaque, usize, usize) callconv(.c) ?*anyopaque,
    deallocate: ?*const fn (?*anyopaque, *anyopaque) callconv(.c) void,
};
const Chunk = struct {
    storage: [*]u8,
    live: u64 = 0,
    burned: u64 = 0,
    generation: [width]u32 = @splat(0),
    owner: [width]i32 = @splat(-1),
    position: [width]usize = @splat(0),
};
const Owner = std.ArrayList(u64);
const Store = struct {
    config: Config,
    chunks: std.ArrayList(*Chunk) = .empty,
    available: std.ArrayList(u32) = .empty,
    owners: []Owner,
    count: usize = 0,

    fn allocator(self: *Store) Allocator {
        return .{ .ptr = self, .vtable = &.{
            .alloc = hostAlloc,
            .resize = Allocator.noResize,
            .remap = Allocator.noRemap,
            .free = hostFree,
        } };
    }

    fn object(self: *Store, index: usize) *anyopaque {
        return self.chunks.items[index / width].storage + (index % width) * self.config.object_size;
    }

    fn validIndex(self: *Store, token: u64) ?usize {
        const low: u32 = @truncate(token);
        if (low == 0) return null;
        const index: usize = low - 1;
        if (index / width >= self.chunks.items.len) return null;
        const chunk = self.chunks.items[index / width];
        const slot: u6 = @intCast(index % width);
        if (chunk.live & (@as(u64, 1) << slot) == 0 or chunk.generation[slot] != token >> 32) return null;
        return index;
    }

    fn grow(self: *Store) !void {
        // index+1 must fit in 32 bits; one final partial chunk is unused.
        if (self.chunks.items.len >= std.math.maxInt(u32) / width) return error.OutOfMemory;
        const host_allocator = self.allocator();
        const count = self.chunks.items.len + 1;
        try self.chunks.ensureTotalCapacity(host_allocator, count);
        try self.available.ensureTotalCapacity(host_allocator, count);
        const chunk = try host_allocator.create(Chunk);
        errdefer host_allocator.destroy(chunk);
        const bytes = try std.math.mul(usize, self.config.object_size, width);
        const memory = self.config.allocate.?(self.config.context, bytes, self.config.object_alignment) orelse return error.OutOfMemory;
        chunk.* = .{ .storage = @ptrCast(memory) };
        self.available.appendAssumeCapacity(@intCast(self.chunks.items.len));
        self.chunks.appendAssumeCapacity(chunk);
    }
};

fn hostAlloc(context: *anyopaque, size: usize, alignment: Alignment, _: usize) ?[*]u8 {
    const store: *Store = @ptrCast(@alignCast(context));
    return @ptrCast(store.config.allocate.?(store.config.context, size, alignment.toByteUnits()) orelse return null);
}
fn hostFree(context: *anyopaque, memory: []u8, _: Alignment, _: usize) void {
    const store: *Store = @ptrCast(@alignCast(context));
    store.config.deallocate.?(store.config.context, memory.ptr);
}

fn create(config_: ?*const Config) callconv(.c) ?*Store {
    const config = (config_ orelse return null).*;
    if (config.allocate == null or config.deallocate == null or config.object_size == 0 or
        !std.math.isPowerOfTwo(config.object_alignment) or config.object_size % config.object_alignment != 0 or
        config.owner_count > std.math.maxInt(i32) or config.generation_max == 0) return null;
    const raw = config.allocate.?(config.context, @sizeOf(Store), @alignOf(Store)) orelse return null;
    const store: *Store = @ptrCast(@alignCast(raw));
    store.* = .{ .config = config, .owners = &.{} };
    store.owners = store.allocator().alloc(Owner, config.owner_count) catch {
        config.deallocate.?(config.context, store);
        return null;
    };
    for (store.owners) |*owner| owner.* = .empty;
    return store;
}

fn insert(store_: ?*Store, owner: c_int, token: *u64) callconv(.c) ?*anyopaque {
    token.* = 0;
    const store = store_ orelse return null;
    if (owner < -1 or (owner >= 0 and @as(usize, @intCast(owner)) >= store.owners.len)) return null;
    // Reserve everything before publishing a slot. Erase never allocates.
    if (owner >= 0) store.owners[@intCast(owner)].ensureUnusedCapacity(store.allocator(), 1) catch return null;
    if (store.available.items.len == 0) store.grow() catch return null;
    const chunk_id = store.available.items[store.available.items.len - 1];
    const chunk = store.chunks.items[chunk_id];
    const slot: u6 = @intCast(@ctz(~(chunk.live | chunk.burned)));
    const index = @as(usize, chunk_id) * width + slot;
    chunk.generation[slot] += 1;
    token.* = (@as(u64, chunk.generation[slot]) << 32) | (index + 1);
    chunk.owner[slot] = owner;
    if (owner >= 0) {
        const list = &store.owners[@intCast(owner)];
        chunk.position[slot] = list.items.len;
        list.appendAssumeCapacity(token.*);
    }
    chunk.live |= @as(u64, 1) << slot;
    if (chunk.live | chunk.burned == std.math.maxInt(u64)) _ = store.available.pop();
    store.count += 1;
    const object = store.object(index);
    @memset(@as([*]u8, @ptrCast(object))[0..store.config.object_size], 0);
    return object;
}

fn get(store_: ?*Store, token: u64) callconv(.c) ?*anyopaque {
    const store = store_ orelse return null;
    return store.object(store.validIndex(token) orelse return null);
}
fn at(store_: ?*Store, index: usize, token: *u64) callconv(.c) ?*anyopaque {
    token.* = 0;
    const store = store_ orelse return null;
    if (index / width >= store.chunks.items.len) return null;
    const chunk = store.chunks.items[index / width];
    const slot: u6 = @intCast(index % width);
    if (chunk.live & (@as(u64, 1) << slot) == 0) return null;
    token.* = (@as(u64, chunk.generation[slot]) << 32) | (index + 1);
    return store.object(index);
}
fn owned(store_: ?*Store, owner: c_int, position: usize, token: *u64) callconv(.c) ?*anyopaque {
    token.* = 0;
    const store = store_ orelse return null;
    if (owner < 0 or @as(usize, @intCast(owner)) >= store.owners.len) return null;
    const list = store.owners[@intCast(owner)].items;
    if (position >= list.len) return null;
    token.* = list[position];
    return get(store, token.*);
}
fn capacity(store_: ?*Store) callconv(.c) usize {
    return if (store_) |store| store.chunks.items.len * width else 0;
}
fn erase(store_: ?*Store, token: u64) callconv(.c) c_int {
    const store = store_ orelse return 0;
    const index = store.validIndex(token) orelse return 0;
    const chunk = store.chunks.items[index / width];
    const slot: u6 = @intCast(index % width);
    const owner = chunk.owner[slot];
    if (owner >= 0) {
        const list = &store.owners[@intCast(owner)];
        const position = chunk.position[slot];
        _ = list.swapRemove(position);
        if (position < list.items.len) {
            const moved = store.validIndex(list.items[position]).?;
            store.chunks.items[moved / width].position[moved % width] = position;
        }
    }
    const was_full = chunk.live | chunk.burned == std.math.maxInt(u64);
    chunk.live &= ~(@as(u64, 1) << slot);
    if (chunk.generation[slot] == store.config.generation_max) chunk.burned |= @as(u64, 1) << slot;
    if (was_full and chunk.live | chunk.burned != std.math.maxInt(u64)) store.available.appendAssumeCapacity(@intCast(index / width));
    store.count -= 1;
    return 1;
}
fn destroy(store_: ?*Store) callconv(.c) c_int {
    const store = store_ orelse return 1;
    if (store.count != 0) return 0;
    const config = store.config;
    const allocator = store.allocator();
    for (store.chunks.items) |chunk| {
        config.deallocate.?(config.context, chunk.storage);
        allocator.destroy(chunk);
    }
    store.chunks.deinit(allocator);
    store.available.deinit(allocator);
    for (store.owners) |*list| list.deinit(allocator);
    allocator.free(store.owners);
    config.deallocate.?(config.context, store);
    return 1;
}
comptime {
    @export(&create, .{ .name = "gw_slot_create", .visibility = .hidden });
    @export(&insert, .{ .name = "gw_slot_insert", .visibility = .hidden });
    @export(&get, .{ .name = "gw_slot_get", .visibility = .hidden });
    @export(&at, .{ .name = "gw_slot_at", .visibility = .hidden });
    @export(&owned, .{ .name = "gw_slot_owned", .visibility = .hidden });
    @export(&capacity, .{ .name = "gw_slot_capacity", .visibility = .hidden });
    @export(&erase, .{ .name = "gw_slot_erase", .visibility = .hidden });
    @export(&destroy, .{ .name = "gw_slot_destroy", .visibility = .hidden });
}
