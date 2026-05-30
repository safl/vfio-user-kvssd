const std = @import("std");

// build.zig.zon is the single source of truth for the version. The release
// workflow overrides it with the git tag via -Dversion; local/CI builds fall
// back to this so the two cannot silently drift.
const zon = @import("build.zig.zon");

const vfu = "subprojects/libvfio-user";
const jsonc = "subprojects/json-c";
const jsonc_cfg = "third_party/json-c-config";

const vfu_lib_sources = [_][]const u8{
    "btree.c",
    "dma.c",
    "fd_cache.c",
    "irq.c",
    "libvfio-user.c",
    "migration.c",
    "pci.c",
    "pci_caps.c",
    "tran.c",
    "tran_sock.c",
};

// json-c is libvfio-user's only external dependency. We vendor it and compile
// from source so the result is self-contained (no runtime json-c). libjson.c
// is the shared-object aggregator and is intentionally omitted.
const jsonc_sources = [_][]const u8{
    "arraylist.c",
    "debug.c",
    "json_c_version.c",
    "json_object.c",
    "json_object_iterator.c",
    "json_patch.c",
    "json_pointer.c",
    "json_tokener.c",
    "json_util.c",
    "json_visit.c",
    "linkhash.c",
    "printbuf.c",
    "random_seed.c",
    "strerror_override.c",
};

const kvssd_sources = [_][]const u8{
    "src/main.c",
    "src/nvme.c",
    "src/kv.c",
};

// Compile libvfio-user + vendored json-c into `mod`. Third-party C uses benign
// constructs that zig cc's Debug UBSan rejects, so it is built with
// -fno-sanitize=undefined (our own code keeps UBSan).
fn addLibvfioUser(b: *std.Build, mod: *std.Build.Module) void {
    // musl lacks <sys/queue.h>; provide a vendored copy (a no-op on glibc).
    mod.addIncludePath(b.path("third_party/musl-compat"));
    mod.addIncludePath(b.path(vfu ++ "/include"));
    mod.addIncludePath(b.path(vfu ++ "/lib"));
    mod.addIncludePath(b.path(jsonc));
    mod.addIncludePath(b.path(jsonc_cfg));

    mod.addCSourceFiles(.{
        .root = b.path(vfu ++ "/lib"),
        .files = &vfu_lib_sources,
        // musl portability: force <sys/types.h> (libvfio-user uses loff_t
        // assuming it is pulled in transitively, as on glibc) and tolerate
        // PAGE_SIZE being redefined over musl's <limits.h> definition.
        .flags = &.{
            "-std=gnu99",        "-D_GNU_SOURCE",          "-DHAVE_LINUX_KCMP_H",
            "-fno-sanitize=undefined", "-include", "third_party/musl-compat/prelude.h",
            "-Wno-macro-redefined",
        },
    });
    mod.addCSourceFiles(.{
        .root = b.path(jsonc),
        .files = &jsonc_sources,
        .flags = &.{ "-std=gnu11", "-D_GNU_SOURCE", "-fno-sanitize=undefined" },
    });
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const want_static = b.option(bool, "static",
        "Link statically (use with -Dtarget=x86_64-linux-musl)") orelse false;
    const version = b.option([]const u8, "version",
        "Version string reported by --version") orelse zon.version;

    // The kvssd device. Strip in release builds (zig strips cross-arch).
    const kvssd_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .strip = optimize != .Debug,
    });
    kvssd_mod.addIncludePath(b.path("include"));
    kvssd_mod.addIncludePath(b.path("src"));
    addLibvfioUser(b, kvssd_mod);
    kvssd_mod.addCSourceFiles(.{
        .files = &kvssd_sources,
        .flags = &.{ "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-include", "third_party/musl-compat/prelude.h" },
    });
    kvssd_mod.addCMacro("VFU_KVSSD_VERSION", b.fmt("\"{s}\"", .{version}));
    const kvssd = b.addExecutable(.{ .name = "vfu_kvssd", .root_module = kvssd_mod });
    if (want_static) kvssd.linkage = .static;
    b.installArtifact(kvssd);

    // NVMe-aware vfio-user client harness (drives the device without QEMU).
    const harness_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    harness_mod.addIncludePath(b.path("include"));
    addLibvfioUser(b, harness_mod);
    harness_mod.addCSourceFiles(.{
        .files = &.{"tests/client_harness.c"},
        .flags = &.{ "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-include", "third_party/musl-compat/prelude.h" },
    });
    const harness = b.addExecutable(.{ .name = "vfu_kvssd_harness", .root_module = harness_mod });
    if (want_static) harness.linkage = .static;
    b.installArtifact(harness);

    // Stress hammer: drives the device's state machine the same way Linux +
    // QEMU vfio-user-pci would under bind/unbind/FLR/CC churn, so wedges
    // reproduce locally instead of in CI.
    const hammer_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    hammer_mod.addIncludePath(b.path("include"));
    addLibvfioUser(b, hammer_mod);
    hammer_mod.addCSourceFiles(.{
        .files = &.{"tests/client_hammer.c"},
        .flags = &.{ "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-include", "third_party/musl-compat/prelude.h" },
    });
    const hammer = b.addExecutable(.{ .name = "vfu_kvssd_hammer", .root_module = hammer_mod });
    if (want_static) hammer.linkage = .static;
    b.installArtifact(hammer);

    // KV handler unit test (no libvfio-user dependency).
    const test_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    test_mod.addIncludePath(b.path("include"));
    test_mod.addIncludePath(b.path("src"));
    test_mod.addCSourceFiles(.{
        .files = &.{ "tests/kv_test.c", "src/kv.c" },
        .flags = &.{ "-std=gnu11", "-Wall", "-Wextra" },
    });
    const kv_test = b.addExecutable(.{ .name = "kv_test", .root_module = test_mod });
    const run_kv_test = b.addRunArtifact(kv_test);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_kv_test.step);
}
