# 测试制品配置

`targets.json` 固定独立 Android HotSpot 测试 JRE（17/ec28559）的归档、libjvm、libjava 和原版 zstd GNU 映像哈希。测试脚本从 `../../config/targets.json` 读取工具链，再叠加这些测试制品信息。

JRE 来源、SQLite 与 zstd 的下载 URL/哈希分别写在 `../scripts/prepare-jre.py`、`prepare-sqlite.py` 和 `prepare-zstd.py`。核心运行时构建不消费本目录。
