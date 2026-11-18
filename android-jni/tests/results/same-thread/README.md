# ARM64 JNI transport comparison

> 本页保留首次固定八上下文夹具的结果和当时边界。当前实现、扩展兼容性和复测结果见 [compatibility](compatibility/README.md)。

## Decision

**Go：继续开发受限的同线程 JNI 后端。No-go：现在替换默认 worker。**

相同生产 JNI 封送/分派路径下，同线程模式在本设备的五类调用、1/8 线程两组中均为十对全部更快。收益足以支持继续工程验证。动态上下文接入、Guest 自建线程、真实 VM suspension 和信号竞态尚未闭环；不能推广到全部模组或全部 JRE。

## Recorded results

设备 V2239A，Android 13，ARM64；固定 Android JRE 17 制品和实验 glibc。性能在 shell 身份运行，应用权限验证另行执行。测试日期 2026-11-15。完整口径和复现命令见 [实验说明](../../same-thread/README.md)。

表中耗时为十个进程结果的中位数，每个进程结果来自批均摊耗时的中位数，单位 µs/调用。减少比例采用逐对比值的中位数，不能用表中两个数直接相除来复算。

| 工作 | 1 线程 worker → domain | 配对耗时减少 | 8 线程 worker → domain | 配对耗时减少 |
| --- | ---: | ---: | ---: | ---: |
| 标量 native | 40.166 → 1.067 | 97.2% | 17.780 → 8.022 | 55.7% |
| Guest 回调 Java | 4.648 → 1.862 | 62.0% | 20.737 → 13.885 | 28.5% |
| 四层重入 | 10.327 → 4.926 | 52.6% | 98.744 → 69.210 | 33.5% |
| 128 B 数组访问/求和 | 3.726 → 1.318 | 64.5% | 37.360 → 11.215 | 68.9% |
| 64 KiB 数组访问/求和 | 59.334 → 33.751 | 42.4% | 160.413 → 43.278 | 72.7% |

**单线程标量基线波动很大**：各轮 worker 中位数为 2.050–69.838 µs，domain 为 0.565–1.366 µs。不能把 40 µs 解释成桥的固定损耗，也不能把 97.2% 当作一般 JNI 收益。八线程各类异步推进，结果混有其他类调用的竞争；它不是每类严格同时起跑的负载。温度、DVFS 和系统后台未受控，本轮不对这些差异作因果归因。

两侧都启用相同实验信号层，数据仅隔离了该夹具中的交接方式。没有获得相对普通 JNI 的绝对桥开销、CPU/能耗下降或游戏 FPS 结论。

## Correctness evidence

- 桌面 Linux 和真机 shell：worker/domain × 1/8 线程矩阵均通过，CheckJNI 开启。
- 应用身份：八线程 domain 模式通过；supervisor UID 10310，子进程通过 linker64 启动并继承应用身份。该 UID 来自 supervisor，不是 native fixture 自行采集。bionic、HotSpot 和 glibc 在同一原生子进程；ART 不在其中。
- 20 对、40 个性能进程：逐对逐线程逐类 checksum 相同，样本完整，退出成功。
- 正常构建的 `android-jni/tests/failures.py` 和 `lifecycle.py` 通过 ASan/UBSan 回归。
- 抽取 VM 启动辅助函数后，原 HotSpot 桌面夹具通过：19,200 次 Java 回调、16,800 次 native 重入、24 次 attach/detach、72 次 Guest→Host 信号、VM 正常销毁。其 GC/JIT 压力不在上述计时中。

## Evidence files

- [summary.json](summary.json)：全部逐对中位数、比值、范围、应用结果及证据哈希。
- [build-manifest.json](build-manifest.json)：本次原生制品、测试 Java、主要源码和 runtime 的哈希及构建命令；包含采集机器绝对路径，路径不是安装约定。它不是完整工具链锁文件。
- `arm64-1-threads.json.gz` / `arm64-8-threads.json.gz`：原始结构化逐批样本、checksum、执行命令和兼容性矩阵，gzip 解压即 JSON。保留这些数据用于重新统计，勿只保留表格。
- [application.txt](application.txt)：最后一次新 APK 的完整兼容性输出；APK SHA256 记录在 summary 中。

原始 stdout/stderr 仍在本地项目 build 目录，对应 capture `20261115T092812479339Z` 和 `20261115T093154393762Z`。源树中的压缩样本足以重新计算表格，不依赖这些 build 路径。

```sh
python3 - <<'PY'
import gzip, json, statistics
from pathlib import Path
base = Path('android-jni/experiments/same-thread/results')
for threads in (1, 8):
    data = json.loads(gzip.decompress((base / f'arm64-{threads}-threads.json.gz').read_bytes()))
    for case in ('scalar', 'callback', 'nested4', 'array128', 'array65536'):
        values = {mode: [statistics.median([
            ns / row['iterations'] for row in pair[mode]['samples']
            if row['case'] == case for ns in row['batch_ns']
        ]) for pair in data['pairs']] for mode in ('worker', 'domain')}
        ratios = [b / a for a, b in zip(values['worker'], values['domain'])]
        print(threads, case,
              [statistics.median(values[m]) / 1000 for m in ('worker', 'domain')],
              'reduction:', 1 - statistics.median(ratios))
PY
```
