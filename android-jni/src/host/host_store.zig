//! Single object root for private Host stores. No second libc or TLS runtime.
const std = @import("std");
pub const panic = std.debug.FullPanic(struct {
    fn call(message: []const u8, address: ?usize) noreturn {
        _ = message;
        _ = address;
        @trap();
    }
}.call);
comptime {
    _ = @import("binding_store.zig");
    _ = @import("slot_store.zig");
}
