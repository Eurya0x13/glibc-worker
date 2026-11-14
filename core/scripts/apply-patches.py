#!/usr/bin/env python3
"""Apply/verify runtime patches independently of an enclosing Git checkout."""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("patches", type=Path, nargs="+")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    patches = [str(p.resolve(strict=True)) for p in args.patches]
    if not source.is_dir() or (source / ".git").exists():
        raise SystemExit("Expected a standalone extracted runtime source tree")
    # git apply otherwise inherits the enclosing checkout's prefix and may
    # silently skip every hunk while returning success. Resolve symlinks before
    # setting the ceiling; inherited worktree/index settings must not escape it.
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(
        GIT_CEILING_DIRECTORIES=str(source.parent),
        GIT_CONFIG_NOSYSTEM="1",
        GIT_CONFIG_GLOBAL=os.devnull,
    )

    def apply(*arguments):
        subprocess.run(
            ["git", "apply", *arguments], cwd=source, env=env, check=True
        )

    if not args.verify_only:
        for patch in patches:
            apply("--check", patch)
            apply(patch)
            # A successful process exit is insufficient: the reverse patch must
            # now match the source, proving that the forward change took effect.
            apply("--reverse", "--check", patch)
    for patch in reversed(patches):
        apply("--reverse", "--check", patch)
    print(f"Runtime patch verification passed: {len(patches)} patches")


if __name__ == "__main__":
    main()
