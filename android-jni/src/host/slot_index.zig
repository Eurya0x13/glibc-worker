//! Optional numeric-key index over live generational slot tokens.
//! Dense traversal and inline singleton buckets avoid linked lists and an
//! allocation for every unique key. The caller owns all referenced objects.
const std = @import("std");
const Allocator = std.mem.Allocator;

const Key = struct { kind: c_int, value: usize };
const Entry = struct { key: Key, token: u64 };
const Bucket = struct {
    first: u64,
    extra: std.ArrayList(u64) = .empty,
};

pub const Index = struct {
    keys: std.AutoHashMapUnmanaged(Key, Bucket) = .empty,
    positions: std.AutoHashMapUnmanaged(u64, usize) = .empty,
    entries: std.ArrayList(Entry) = .empty,

    pub fn candidate(self: *Index, kind: c_int, value: usize, position: usize) u64 {
        const bucket = self.keys.get(.{ .kind = kind, .value = value }) orelse return 0;
        if (position == 0) return bucket.first;
        return if (position - 1 < bucket.extra.items.len) bucket.extra.items[position - 1] else 0;
    }

    pub fn at(self: *Index, position: usize) u64 {
        return if (position < self.entries.items.len) self.entries.items[position].token else 0;
    }

    pub fn insert(self: *Index, allocator: Allocator, kind: c_int, value: usize, token: u64) !void {
        const key = Key{ .kind = kind, .value = value };
        // Reserve before publishing anything. Failed growth may keep spare
        // capacity, but cannot leave a key, token or dense entry behind.
        try self.entries.ensureUnusedCapacity(allocator, 1);
        try self.positions.ensureUnusedCapacity(allocator, 1);
        try self.keys.ensureUnusedCapacity(allocator, 1);
        if (self.keys.getPtr(key)) |bucket| {
            try bucket.extra.ensureUnusedCapacity(allocator, 1);
            bucket.extra.appendAssumeCapacity(token);
        } else {
            self.keys.putAssumeCapacityNoClobber(key, .{ .first = token });
        }
        self.positions.putAssumeCapacityNoClobber(token, self.entries.items.len);
        self.entries.appendAssumeCapacity(.{ .key = key, .token = token });
    }

    pub fn erase(self: *Index, allocator: Allocator, token: u64) void {
        const position = self.positions.fetchRemove(token).?.value;
        const entry = self.entries.swapRemove(position);
        if (position < self.entries.items.len) {
            self.positions.getPtr(self.entries.items[position].token).?.* = position;
        }
        const bucket = self.keys.getPtr(entry.key).?;
        if (bucket.first == token) {
            if (bucket.extra.pop()) |replacement| {
                bucket.first = replacement;
            } else {
                bucket.extra.deinit(allocator);
                std.debug.assert(self.keys.remove(entry.key));
            }
        } else {
            const sibling = std.mem.findScalar(u64, bucket.extra.items, token).?;
            _ = bucket.extra.swapRemove(sibling);
        }
    }

    pub fn deinit(self: *Index, allocator: Allocator) void {
        std.debug.assert(self.entries.items.len == 0);
        std.debug.assert(self.keys.count() == 0 and self.positions.count() == 0);
        self.keys.deinit(allocator);
        self.positions.deinit(allocator);
        self.entries.deinit(allocator);
    }
};
