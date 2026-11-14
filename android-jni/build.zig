//! Host resource-store compilation. CMake still owns C/JNI and SDK packaging.
const std = @import("std");
const builtin = @import("builtin");

comptime {
    if (!std.mem.eql(u8, builtin.zig_version_string, "0.16.0"))
        @compileError("android-jni requires exactly Zig 0.16.0");
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const bindings_only = b.option(bool, "bindings-only", "Build only the binding store for C control tests") orelse false;
    const module = b.createModule(.{
        .root_source_file = b.path(if (bindings_only) "src/host/binding_store.zig" else "src/host/host_store.zig"),
        .target = target,
        .optimize = .ReleaseSafe,
        .pic = true,
        .error_tracing = false,
        .stack_check = false,
        .stack_protector = false,
    });
    const object = b.addObject(.{
        .name = if (bindings_only) "binding_store" else "host_store",
        .root_module = module,
    });
    object.bundle_compiler_rt = false;
    const install = b.addInstallFileWithDir(object.getEmittedBin(), .prefix, if (bindings_only) "binding_store.o" else "host_store.o");
    b.step("host-store", "Compile and install the private Host object").dependOn(&install.step);
    b.getInstallStep().dependOn(&install.step);
}
