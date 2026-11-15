# 1–2 MiB JNI 数组副本缓存

本目录保留 Android ARM64 / Vivo V2239A / Android 13、固定 Android JRE17 的机制与缓存开关对照。主实验在同一个 APK 中执行预渲染测试模组，避免世界渲染和 GPU 压力混入计时。Pojav 集成和测试模组源码继续归外部验证工程；这里保存数组机制相关的原始数值和制品清单。

## 改动与结论

Host 副本缓存下限由 2 MiB 降至 1 MiB，新覆盖区间为 `[1 MiB, 2 MiB)`；2 MiB 原本已缓存。最大数组 8 MiB、8 槽、32 MiB 数据预算不变。缓存持有活跃分配，避免释放后反复退页或解除映射；实际内容复制、首次触页和资源不足时的 JVM 回退仍存在。

在四个新进程中按 off/auto/auto/off 执行，测试进程显式请求立即退页，每进程包含十对交替 direct/bridge 测量。表中为进程内批均摊中位数的跨进程中位数，单位 µs/调用；这是 Elements 调用总耗时，不是孤立的桥开销。

| 大小 | off | auto | 耗时降低 |
| --- | ---: | ---: | ---: |
| 1 MiB | 291.640 | 47.908 | 83.6% |
| 1.5 MiB | 584.708 | 89.423 | 84.7% |
| 2 MiB，已有覆盖对照 | 822.399 | 142.885 | 82.6% |

下限减一字节的对照仍慢（290.091 → 298.522 µs）；直接 JNI 对照在开启桥缓存后仍慢。诊断循环中，未缓存的 1/1.5/2 MiB Elements 每调用约有 257/385/513 次轻微缺页，缓存后中位数为零。Region 向保留缓冲区复制的成本接近缓存 Elements。这些对照支持分配／退页成本解释，不能推导日常游戏平均收益。

另外两进程请求延迟退页，但并非所有计时阶段都保持低缺页，因此不能把它们统称为“快状态”。其后续低缺页的 1 MiB 诊断中，auto 进程直接 JNI / 桥为 56.68 / 54.34 µs，off 进程为 63.36 / 67.52 µs；未观察到明显的缓存回退信号，样本不足以保证所有负载均无回退。1.5/2 MiB 仍有大量缺页。最后一个自然策略的 worker 进程只作兼容验证，不作为 worker/domain 性能比较。

## 分配器机制

独立 Android shell 进程在每个大小下按 decay 1/0/0/1 请求释放策略，比较每次 malloc/copy/free 与保留同一缓冲区。1/1.5 MiB 在立即回收时保留映射但物理页驻留降为零；保留活跃分配则保持 256/384 页驻留并消除重复复制的缺页。2 MiB 在两种策略下 free 后查询范围均不再完整映射（mincore 返回 ENOMEM）；反复分配复制为 513 次轻微缺页，保留分配后为零。失败的 mincore 记录中 resident_pages=0 不是有效驻留测量。

独立进程不能证明应用的缓存准入和策略完全相同。原始自然运行中具体由哪个调用方改变了分配器状态仍未定位；不能把受控的 M_DECAY_TIME=0 复现等同于已找到该调用方。机制源码的固定版本链接见 [缓存契约](../../../../docs/ARRAY_COPY_CACHE.md)。

## 正确性与证据

七个 APK 进程全部通过。每个后端有 312 项基本类型数组检查，覆盖八类型、COMMIT/ABORT、回写、异常身份、GC、12 个同时借用和跨线程归还。两种交接后端均验证内容、线程身份和桥错误计数。三批结束均恢复原 APK、启动器配置和受保护游戏文件。独立 ASan/UBSan 容量及生命周期回归也通过。

- `capture.json.gz` 保存七进程的完整 throughput JSONL、400 行/进程的独立数组诊断、完成记录和原始文件 SHA256；不包含账户、启动器配置或完整游戏日志。
- `summary.json` 保存缓存开关比较、每类结果和解释边界。`worker` 是兼容性检查，因此没有成对比较表。
- `allocator-pages.json` 保存独立分配器三种大小的全部数值及实际运行时源码、二进制和原始文件散列。诊断不链接进产品，仅改变短命测试进程的策略。
- `build-manifest.json` 绑定当时测量的 APK、模组、共享 C workload、JNI 合同、runtime 与源码。它记录的是格式整理前的制品，不能将其中源码散列当作当前 checkout 的散列。含构建机器路径，不是部署路径约定。
- `sha256.json` 绑定本目录发布的证据文件。历史数据保留原值，不与整理后的桌面复核或先前其他大小的基准合并。

从仓库根目录用标准 Python 重算表中桥路径的批均摊中位数：

```python
import gzip
import json
import statistics
from pathlib import Path

root = Path("android-jni/experiments/same-thread/results/array-cache")
data = json.loads(gzip.decompress((root / "capture.json.gz").read_bytes()))
batch = next(b for b in data["batches"] if b["label"] == "midrange")
for size in (1048576, 1572864, 2097152):
    groups = {"off": [], "auto": []}
    for run in batch["runs"]:
        rows = map(json.loads, run["files"]["samples.jsonl"].splitlines())
        samples = [r["batch_wall_ns"] / r["batch_calls"] for r in rows
                   if r["type"] == "sample" and r["domain"] == "worker"
                   and r["case"] == f"array-elements-{size}"]
        groups[run["cache"]].append(statistics.median(samples))
    print(size, {k: statistics.median(v) / 1000 for k, v in groups.items()})
```

原始 schema 用 `domain=worker` 表示桥路径，是否真正跨线程由 metadata.transport 和两个 TID 决定；不要从该字段名推断后端。

## 复现入口

本仓库的 `python3 android-jni/tests/array_cache.py` 验证下限、所有权、槽/预算回退与清理，需先由集成构建准备 Android JNI 头。`android-jni/tests/native/allocator_pages.c` 用 NDK 的 ARM64 Android clang 编译后在隔离 shell 进程运行，参数分别为 1048576、1572864、2097152；每次运行都包含上述两种分配方式和策略序列。

端到端模组实验在外部 Pojav 验证工程的 `bridge-cost-mod/DOMAIN.md` 维护：使用匹配的 native parent 构建 fixture，`build_domain.py --array-contract-source` 指向本仓库 `android-jni/tests/native/array_copy_checks.c`，随后 `domain_device.py --array-profile midrange --array-diagnostic`。四轮主对照使用 `--modes domain,domain,domain,domain --cache-states off,auto,auto,off --allocator-decay 0`；延迟策略对照为 `domain,domain` / `auto,off` / `1`；worker 兼容轮为 `worker` / `auto` / `natural`。每批使用新输出目录，结束后恢复原包和配置。测试模组未计时的语义检查先于预热，独立诊断在 throughput 阶段之后执行，两套数字不能混用。
