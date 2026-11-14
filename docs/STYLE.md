# 代码格式

维护源码使用 UTF-8、LF、空格缩进，并保留一个文件末尾换行。编辑器默认配置在根目录 `.editorconfig`，Git 换行规则在 `.gitattributes`。

| 语言 | 规则 |
| --- | --- |
| C / 头文件 | `.clang-format`：Google 基准、2 空格、80 列、case 缩进，保留 include 顺序 |
| Java | `.clang-format`（Java 段）：Google Java 风格、2 空格、100 列 |
| Bash | shfmt：2 空格、case 缩进、跨行二元操作符；长命令按参数组手动拆行，维护脚本不超过 100 列 |
| Python | Ruff formatter，80 列、双引号，对齐 Google Python 风格；配置在 `pyproject.toml` |
| Kotlin Gradle DSL | Google Kotlin 风格、4 空格、100 列，插件、仓库及长列表按块展开 |
| JSON | 2 空格，与生成工具一致 |

C / 头文件 / Java 的函数、类型定义之间保留一个空行，由
`SeparateDefinitionBlocks: Always` 自动维护。函数内部按逻辑阶段留空行，避免把检查、分配、调用与清理挤在一起；Python、Bash 与 Kotlin 同样遵守这一分块约定。

C / 头文件 / Java 的 `if`、`else`、`for`、`while` 语句体一律使用花括号，即使只有一条语句；C 由 `InsertBraces: true` 自动维护，Java 由脚本补齐后经 clang-format 归一化。手工维护的宏区域同样遵守该规则。

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

格式脚本只遍历维护代码；第三方许可证、Gradle wrapper、下载源码、构建缓存与自动生成 `.inc` 不做手工格式化。生成代码需要调整时修改生成器。Kotlin DSL 当前按上述规则维护，未宣称经过 Kotlin formatter 检查。
