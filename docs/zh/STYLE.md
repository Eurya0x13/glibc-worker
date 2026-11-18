# 代码格式

[English](../en/STYLE.md) | [简体中文](../zh/STYLE.md)

维护源码使用 UTF-8、LF、空格缩进，并保留一个文件末尾换行。编辑器默认配置在根目录 `.editorconfig`，Git 换行规则在 `.gitattributes`。

| 语言 | 规则 |
| --- | --- |
| C / 头文件 | `.clang-format`：Google 基准、2 空格、80 列、case 缩进，保留 include 顺序 |
| Java | `.clang-format`（Java 段）：Google Java 风格、2 空格、100 列 |
| Bash | shfmt：2 空格、case 缩进、跨行二元操作符；长命令按参数组手动拆行，维护脚本不超过 100 列 |
| Python | Ruff formatter，80 列、双引号，对齐 Google Python 风格；配置在 `pyproject.toml` |
| Kotlin Gradle DSL | Google Kotlin 风格、4 空格、100 列，插件、仓库及长列表按块展开 |
| JSON | 2 空格，与生成工具一致 |
| Zig | 固定 Zig 0.16.0 的 `zig fmt`，4 空格 |

维护 Host 存储源码及构建 android-jni 需要 Zig 0.16.0；格式脚本调用 `zig fmt`。core 和 bionic-dlopen 自身的构建不依赖 Zig。

C / 头文件 / Java 的函数、类型定义之间保留一个空行，由
`SeparateDefinitionBlocks: Always` 自动维护。源码链接中的不可拆分 URL 可单独占行并超过列宽，保持链接可直接访问；其余注释遵循相应列宽。

函数内部按逻辑阶段留空行，避免把检查、分配、调用与清理挤在一起；Python、Bash 与 Kotlin 同样遵守这一分块约定。

C / 头文件 / Java 的 `if`、`else`、`for`、`while` 语句体一律使用花括号，即使只有一条语句；C 由 `InsertBraces: true` 自动维护，Java 手动补齐后经 clang-format 归一化。手工维护的宏区域同样遵守该规则。

少量拼接 JNI 类型名的 C 宏会被 clang-format 错判缩进。这些宏只在定义范围内使用
`clang-format off/on`，手工保持 2 空格缩进、80 列内、续行反斜杠对齐及逻辑分段；调用处和其余代码仍自动格式化。

```sh
# 更新格式。
python3 scripts/format.py
# 检查格式，不修改文件。
python3 scripts/format.py --check
# 补充检查 Bash 的常见错误。
shellcheck -S warning scripts/*.sh jvm-jni/tests/scripts/*.sh jvm-jni/tests/smoke/run.sh jvm-jni/tests/coverage/run.sh
```

需要安装 `clang-format`、`shfmt` 和 `ruff`；脚本从 PATH 或本地 `scripts/build/tools/` 查找。推荐固定格式工具版本，例如 clang-format 21.1.8、shfmt 3.12.0 和 Ruff 0.17.4。shfmt 来源为 [官方发布页](https://github.com/mvdan/sh/releases/tag/v3.12.0)。

格式脚本只遍历维护代码；第三方许可证、Gradle wrapper、下载源码、构建缓存与自动生成 `.inc` 不做手工格式化。生成代码需要调整时修改生成器。补丁运行时的手写 `adopt-nptl.inc`、`signal-guard.inc` 和 `signal-registry.inc` 由格式脚本显式纳入 C 格式检查。Kotlin DSL 当前按上述规则维护，未宣称经过 Kotlin formatter 检查。

## 文档语言

工作区及各项目各自维护对应的 `docs/en/` 与 `docs/zh/`，文件名保持一致，
提供相互切换的语言导航。项目根 README 使用完整英文正文，保留两种语言入口，
并与 `docs/en/README.md` 保持同步。同一次修改同步更新两种语言，
保持接口名、命令、单位、错误语义和验证边界一致。复核过的历史证据归档到所属项目的
`tests/results/`，保留原语言和原始文件内容。

从工作区根运行 `python3 scripts/check-docs.py`，检查成对文件名、语言导航、
本地链接/锚点及可执行示例。自动检查不能代替译文语义审阅。
