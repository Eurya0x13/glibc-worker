# 核心目标配置

`targets.json` 只包含 `x86_64` 和 `arm64-v8a` 的 GNU triplet、Guest 编译器、NDK triplet 和 GNU loader 名称。

JRE、SQLite、zstd 等验证制品的固定来源在 `tests/config/` 和 `tests/scripts/`。它们不参与核心工具链选择。`scripts/target.py` 不依赖测试配置；测试工具在其上叠加自己的制品信息。
