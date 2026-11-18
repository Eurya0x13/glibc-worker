# Code style

[English](../en/STYLE.md) | [简体中文](../zh/STYLE.md)

Maintained sources use UTF-8, LF, spaces and one final newline. Defaults live in `.editorconfig` and Git newline rules in `.gitattributes`.

| Language | Rules |
| --- | --- |
| C / headers | `.clang-format`: Google base, 2 spaces, 80 columns, indented cases, preserved include order |
| Java | Java section of `.clang-format`: Google style, 2 spaces, 100 columns |
| Bash | shfmt: 2 spaces, indented cases, binary operators on continuation lines; group long arguments manually, at most 100 columns |
| Python | Ruff formatter, 80 columns, double quotes, Google-style layout; `pyproject.toml` |
| Kotlin Gradle DSL | Google Kotlin style, 4 spaces, 100 columns; expand plugins, repositories and long lists |
| JSON | 2 spaces, matching generators |
| Zig | Pinned Zig 0.16.0 `zig fmt`, 4 spaces |

Maintaining Host stores and building android-jni require Zig 0.16.0. The formatter invokes `zig fmt`; core and bionic-dlopen builds themselves do not require Zig.

Separate C/header/Java function and type definitions by one blank line (`SeparateDefinitionBlocks: Always`). Indivisible source URLs may occupy their own line beyond the limit; keep other comments within the configured width.

Separate logical phases within functions with blank lines instead of packing validation, allocation, calls and cleanup together. Apply this to Python, Bash and Kotlin too.

Always brace C/header/Java if/else/for/while bodies, including single statements. C uses `InsertBraces: true`; add Java braces manually before formatting. Hand-maintained macros follow the same rule.

A few JNI token-pasting macros confuse clang-format. Disable it only within those macro definitions and manually preserve 2-space indentation, 80-column lines, aligned continuation backslashes and logical spacing. Calls and surrounding code remain automatically formatted.

```sh
# Format maintained sources.
python3 scripts/format.py
# Check without modifying files.
python3 scripts/format.py --check
# Check common Bash mistakes.
shellcheck -S warning scripts/*.sh jvm-jni/tests/scripts/*.sh jvm-jni/tests/smoke/run.sh jvm-jni/tests/coverage/run.sh
```

Install clang-format, shfmt and ruff on PATH or under `scripts/build/tools/`. Recommended pinned versions are clang-format 21.1.8, shfmt 3.12.0 and Ruff 0.17.4. Obtain shfmt from its [official release](https://github.com/mvdan/sh/releases/tag/v3.12.0).

The script visits maintained code only, excluding third-party licenses, Gradle wrappers, downloaded sources, caches and generated `.inc` files. Change generators rather than generated code. Handwritten `adopt-nptl.inc`, `signal-guard.inc` and `signal-registry.inc` are explicitly included as C. Kotlin DSL follows these rules manually; no Kotlin formatter verification is claimed.

## Documentation languages

The workspace and each project own matching `docs/en/` and `docs/zh/` trees.
Use identical filenames and reciprocal language navigation. Project-root README
files contain the full English version, with links to both languages; keep them
in sync with `docs/en/README.md`. Update both versions in the same change,
preserving API names, commands, units, error semantics and validation limits.
Keep reviewed historical evidence under the owning project's `tests/results/`.
Preserve the original language and raw file contents.

Run `python3 scripts/check-docs.py` from the workspace root to check paired
filenames, language navigation, local links/anchors and executable examples.
The check does not replace semantic review of translations.
