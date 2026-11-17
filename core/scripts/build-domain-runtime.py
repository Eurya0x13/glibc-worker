"""Build an isolated glibc with the NPTL adoption entry points."""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path
from context_mask import patch_context_masks
from wait_mask import patch_wait_masks
from signal_send import patch_signal_sends
from private_signals import patch_private_signals
from mask_lifecycle import patch_mask_lifecycle

source = Path(__file__).resolve().parent
core = source.parent
fragments = core / "src/runtime"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--abi", choices=["x86_64", "arm64-v8a"], required=True)
parser.add_argument("--jobs", type=int, default=4)
args = parser.parse_args()
base = core / "build/glibc-runtime" / args.abi
root_file = base / "build-root.txt"
if not root_file.is_file():
    parser.error("Build the normal matched core runtime first")
base_source = Path(root_file.read_text().strip()) / "src"
target = json.loads((core / "config/targets.json").read_text())[args.abi]
inputs = [
    Path(__file__),
    fragments / "adopt-nptl.inc",
    fragments / "signal-guard.inc",
    fragments / "signal-send.h",
    fragments / "signal-send.inc",
    fragments / "signal-send-kill.c",
    fragments / "signal-mask.inc",
    fragments / "context-mask.inc",
    fragments / "wait-mask.inc",
    fragments / "wait-mask.h",
    fragments / "jump-hook.inc",
    core / "scripts/context_mask.py",
    core / "scripts/wait_mask.py",
    core / "scripts/signal_send.py",
    core / "scripts/private_signals.py",
    core / "scripts/mask_lifecycle.py",
    base / "patch-hashes.txt",
    base / "source-sha256.txt",
    core / "config/targets.json",
]
key = hashlib.sha256(b"".join(p.read_bytes() for p in inputs)).hexdigest()[:16]
root = core / "work" / f"same-thread-{args.abi}-{key}"
src, obj = root / "src", root / "obj"
runtime = core / "build/domain-runtime" / args.abi / key
runtime.mkdir(parents=True, exist_ok=True)


def replace(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise SystemExit(f"Patch context mismatch: {path}")
    path.write_text(text.replace(old, new))


if not (root / "patched").exists():
    if src.exists():
        shutil.rmtree(src)
    shutil.copytree(base_source, src)
    patch_private_signals(src, replace)
    create = src / "nptl/pthread_create.c"
    replace(
        create,
        "static int _Noreturn start_thread (void *arg);",
        "static void (*gw_thread_hook) (int);\n"
        "int __gw_domain_thread_hook (void (*hook) (int)) {\n"
        "  if (hook == NULL || gw_thread_hook != NULL) return EINVAL;\n"
        "  gw_thread_hook = hook; return 0;\n}\n"
        "static int _Noreturn start_thread (void *arg);",
    )
    replace(
        create,
        "  if (__glibc_unlikely (GLRO (dl_debug_mask) & DL_DEBUG_TLS))",
        "  if (gw_thread_hook) gw_thread_hook (1);\n\n"
        "  if (__glibc_unlikely (GLRO (dl_debug_mask) & DL_DEBUG_TLS))",
    )
    replace(
        create,
        "  /* Tell __pthread_kill_internal that this thread is about to exit.",
        "  if (gw_thread_hook) gw_thread_hook (0);\n\n"
        "  /* Tell __pthread_kill_internal that this thread is about to exit.",
    )
    create.write_text(create.read_text() + '\n#include "gw-adopt.inc"\n')
    shutil.copyfile(fragments / "adopt-nptl.inc", src / "nptl/gw-adopt.inc")
    versions = src / "nptl/Versions"
    text = versions.read_text()
    text = text.replace(
        "  GLIBC_PRIVATE {",
        "  GLIBC_PRIVATE {\n"
        "    __gw_domain_alloc; __gw_domain_bind;\n"
        "    __gw_domain_run; __gw_domain_finish; __gw_domain_free;\n"
        "    __gw_domain_thread_hook; __gw_domain_capabilities;",
        1,
    )
    versions.write_text(text)
    versions = src / "nptl/Versions"
    replace(
        versions,
        "__gw_domain_thread_hook; __gw_domain_capabilities;",
        "__gw_domain_thread_hook; __gw_domain_capabilities; __gw_domain_mask_bind;",
    )
    patch_signal_sends(src, fragments, replace)
    patch_mask_lifecycle(src, replace)
    shutil.copyfile(
        fragments / "signal-mask.inc", src / "nptl/gw-signal-mask.inc"
    )
    shutil.copyfile(
        fragments / "context-mask.inc", src / "nptl/gw-context-mask.inc"
    )
    shutil.copyfile(fragments / "wait-mask.inc", src / "nptl/gw-wait-mask.inc")
    shutil.copyfile(fragments / "wait-mask.h", src / "include/gw-wait-mask.h")
    mask = src / "nptl/pthread_sigmask.c"
    replace(
        mask,
        "#include <shlib-compat.h>",
        '#include <shlib-compat.h>\n#include "gw-signal-mask.inc"\n'
        '#include "gw-context-mask.inc"\n#include "gw-wait-mask.inc"',
    )
    replace(
        mask,
        "  sigset_t local_newmask;",
        "  if (gw_logical_mask && *gw_mask_active)\n"
        "    return gw_domain_sigmask (how, newmask, oldmask);\n"
        "  sigset_t local_newmask;",
    )
    patch_context_masks(src)
    patch_wait_masks(src)
    # Keep each libc's native register, mask and fortified restore code.
    shutil.copyfile(
        fragments / "jump-hook.inc", src / "setjmp/gw-jump-hook.inc"
    )
    replace(
        src / "setjmp/sigjmp.c",
        "#include <signal.h>",
        '#include <signal.h>\n#include <errno.h>\n#include "gw-jump-hook.inc"',
    )
    replace(
        src / "setjmp/sigjmp.c",
        "  return 0;",
        "  __gw_domain_jump_event (env, 0, 0);\n  return 0;",
    )
    replace(
        src / "setjmp/longjmp.c",
        "#include <signal.h>",
        "#include <signal.h>\n"
        "extern void __gw_domain_jump_event (void *, int, int) attribute_hidden;",
    )
    replace(
        src / "setjmp/longjmp.c",
        "  /* Perform any cleanups needed by the frames being unwound.  */",
        "#ifdef GW_DOMAIN_CHECKED_JUMP\n"
        "  __gw_domain_jump_event (env, val, 2);\n"
        "#else\n  __gw_domain_jump_event (env, val, 1);\n#endif\n"
        "  /* Perform any cleanups needed by the frames being unwound.  */",
    )
    replace(
        src / "debug/longjmp_chk.c",
        "#include <setjmp/longjmp.c>",
        "#define GW_DOMAIN_CHECKED_JUMP 1\n#include <setjmp/longjmp.c>",
    )
    with (src / "setjmp/Versions").open("a") as stream:
        stream.write(
            "\nlibc {\n  GLIBC_PRIVATE {\n    __gw_domain_jump_hook;\n  }\n}\n"
        )
    versions = src / "signal/Versions"
    replace(
        versions,
        "  GLIBC_PRIVATE {",
        "  GLIBC_PRIVATE {\n    __gw_domain_reserve_signals; __gw_domain_sigaction_hook;",
    )
    shutil.copyfile(
        fragments / "signal-guard.inc", src / "signal/gw-signal-guard.inc"
    )
    action = src / "sysdeps/unix/sysv/linux/libc_sigaction.c"
    replace(
        action,
        "#include <errno.h>",
        "#include <errno.h>\n#include <signal.h>\n#include <gw-signal-guard.inc>",
    )
    replace(
        action,
        "  int result;",
        "  if (gw_guest_sigaction)\n"
        "    {\n"
        "      int handled = 0;\n"
        "      int status = gw_guest_sigaction (sig, act, oact, &handled);\n"
        "      if (handled) return status;\n"
        "    }\n"
        "  if (gw_signal_is_host_owned (sig))\n"
        "    {\n"
        "      __set_errno (EBUSY);\n"
        "      return -1;\n"
        "    }\n"
        "  int result;",
    )
    attributes = src / "sysdeps/nptl/internaltypes.h"
    attributes.write_text(
        attributes.read_text() + "\n#define ATTR_FLAG_GW_DOMAIN 0x40000000\n"
    )
    replace(
        src / "nptl/pthread_cancel.c",
        "  volatile struct pthread *pd = (volatile struct pthread *) th;",
        "  volatile struct pthread *pd = (volatile struct pthread *) th;\n"
        "  if (pd->flags & ATTR_FLAG_GW_DOMAIN)\n    return ENOTSUP;",
    )
    replace(
        src / "nptl/pthread_setcancelstate.c",
        "  self = THREAD_SELF;",
        "  self = THREAD_SELF;\n"
        "  if ((self->flags & ATTR_FLAG_GW_DOMAIN) && state == PTHREAD_CANCEL_ENABLE)\n"
        "    return ENOTSUP;",
    )
    replace(
        src / "nptl/nptl_setxid.c",
        "  xidcmd = cmdp;",
        "  list_t *domain_node;\n"
        "  list_for_each (domain_node, &GL (dl_stack_user))\n"
        "    {\n"
        "      struct pthread *domain = list_entry (domain_node, struct pthread, list);\n"
        "      if (domain->flags & ATTR_FLAG_GW_DOMAIN)\n"
        "        {\n"
        "          lll_unlock (GL (dl_stack_cache_lock), LLL_PRIVATE);\n"
        "          __set_errno (ENOTSUP);\n"
        "          return -1;\n"
        "        }\n"
        "    }\n"
        "  xidcmd = cmdp;",
    )
    (root / "patched").touch()

obj.mkdir(parents=True, exist_ok=True)
environment = {
    **os.environ,
    "CC": target["guest_cc"],
    "CXX": target["guest_cc"].removesuffix("gcc") + "g++",
}
if not (obj / "Makefile").exists():
    command = [
        str(src / "configure"),
        "--build="
        + subprocess.check_output(["gcc", "-dumpmachine"], text=True).strip(),
        "--host=" + target["gnu_triplet"],
        "--prefix=/data/data/com.termux/files/usr/glibc",
        "--libdir=/data/data/com.termux/files/usr/glibc/lib",
        "--enable-bind-now",
        "--disable-multi-arch",
        "--disable-nscd",
        "--disable-profile",
        "--disable-werror",
        "--disable-default-pie",
        "--enable-stack-protector=strong",
    ]
    if args.abi == "x86_64":
        command.append("--enable-cet")
    with (runtime / "configure.log").open("w") as log:
        subprocess.run(
            command,
            cwd=obj,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
with (runtime / "build.log").open("w") as log:
    subprocess.run(
        ["make", "-C", str(obj), f"-j{args.jobs}"],
        stdout=log,
        stderr=subprocess.STDOUT,
        check=True,
    )
for origin, name in [
    ("elf/ld.so", target["loader"]),
    ("libc.so", "libc.so.6"),
    ("math/libm.so", "libm.so.6"),
    ("nptl/libpthread.so", "libpthread.so.0"),
    ("dlfcn/libdl.so", "libdl.so.2"),
    ("rt/librt.so", "librt.so.1"),
]:
    shutil.copyfile(obj / origin, runtime / name)
(runtime / "build-root.txt").write_text(str(root) + "\n")
(runtime / "manifest.json").write_text(
    json.dumps(
        {
            "base": str(base),
            "abi": args.abi,
            "key": key,
            "domain_abi": 1,
            "capabilities": [
                "adopted-carriers",
                "host-signal-reservation",
                "reserved-signal-masks",
                "context-signal-masks",
                "reserved-signal-waits",
                "guest-sigaction-hook",
                "guest-native-jump-hook",
                "optional-guest-signal-send-v1",
                "private-nptl-signals-63-64",
                "logical-mask-inheritance-and-notify-v1",
            ],
            "sha256": {
                str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in inputs
            },
        },
        indent=2,
    )
    + "\n"
)
print(runtime)
