# Same-thread JNI compatibility and paired performance

本报告对应扩展后的动态上下文后端；不是上一轮固定八个上下文夹具的数据。
测试日期为 2026-11-15。复现入口及明确支持边界见 [实验说明](../../README.md)。

## Compatibility contract

采用已约定的 Host 信号所有权方案：VM 必需信号由 Host 管理，Guest 冲突注册和查询返回 `EBUSY`。同线程路径仅承担 Host 同步发起的调用；Guest 自建线程继续使用原反向 JNI 路径。它不是完整 POSIX 信号虚拟化。

本轮实现：

- 首次 native 调用动态创建上下文，正常 pthread-key 析构回收；三轮 32 个同时存活的 Java 线程不依赖夹具预绑定。
- 真实生产 JNI 表、封送、引用/ID 存储和 libffi 跳板；复用十组 conformance 方法/字段/数组矩阵。
- 32 个 Guest pthread 的普通/daemon 附着、Java 回调、native 重入、detach 和 join。
- NPTL 线程入口/出口登记 Guest TLS；Host 信号登记采用可复用的稳定分页记录。
- 部分初始化失败撤销信号登记并释放栈，TCB 分配故障后同一 Java 线程重试成功。
- 每次 Host 信号注册绑定不可变代次，避免并发替换时混用内核掩码与处理器；单独测试内核已重置一次性处理器、路由尚未执行的窗口。
- 旧 Android HotSpot 的 exec 子进程复用已有堆指针标记兼容处理，使 JFR 启动成功；只设置应用 manifest 不足以覆盖 exec。

正常进程要求命中 **234 个接口入口**，另在隔离进程验证 FatalError 的预期 SIGABRT，合计覆盖生成清单的 **230 个 JNI 和 5 个 VM 入口**。明确拒绝的操作也计入入口覆盖，不能将入口覆盖解释为全部参数组合或语义均受支持。

每次兼容性运行开启 CheckJNI，验证三轮共 96 个 Java 线程、32 层重入、960 次 monitor 更新、独立 TLS 和 Java ThreadLocal、字符串/数组/引用/异常、装载/卸载、上下文回收以及 VM 销毁。native 阻塞时执行 20 轮栈采样/GC；JFR 开始/采样/结束且实际 VM SR 信号转发计数非零。非零计数不证明每次 SR 中断都发生在 Guest TLS 下。

## Results

桌面 x86_64 和 Android ARM64 shell 的 worker/domain × 1/8 线程矩阵均通过，FatalError 在两种后端均按预期触发 SIGABRT。应用身份下八载体 domain 模式通过，234 个正常接口入口、96 个 Java 线程、32 个 Guest pthread、上下文创建/回收计数一致及 JFR 信号断言全部通过。supervisor UID 为 10310，原生子进程继承应用身份；ART 不在双 libc 子进程中。

独立信号合同在桌面和真机通过，包括 1,000 次并发替换及一次性处理器发布窗口。HotSpot 压力夹具在两平台通过 19,200 次回调、16,800 次重入、24 次 attach/detach、24 组析构、12 次 Guest 最外层 pthread_exit 和 VM 销毁。正常后端失败路径与生命周期的 ASan/UBSan 回归通过。

最终原生制品的 1/8 线程各十对 A/B 完成，40 个性能进程均退出成功、样本完整、逐对 checksum 一致。**结论：支持继续开发同线程后端；不据此替换默认 worker 或宣称通用模组兼容。**

单位 µs/调用。耗时列为各进程批均摊中位数的汇总中位数；减少比例为逐对 domain/worker 比值的中位数，不能直接用耗时两列相除复算。

| 工作 | 1 线程 worker → domain | 配对耗时减少 | 更快对数 | 8 线程 worker → domain | 配对耗时减少 | 更快对数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 标量 native | 49.246 → 1.151 | 96.3% | 10/10 | 16.384 → 8.124 | 50.2% | 10/10 |
| Guest 回调 Java | 4.272 → 1.954 | 58.7% | 10/10 | 23.526 → 13.723 | 43.8% | 10/10 |
| 四层重入 | 9.504 → 5.023 | 46.7% | 10/10 | 85.023 → 66.195 | 22.4% | 10/10 |
| 128 B 数组访问/求和 | 4.020 → 1.337 | 66.7% | 10/10 | 33.521 → 12.452 | 62.5% | 10/10 |
| 64 KiB 数组访问/求和 | 71.116 → 34.989 | 52.7% | 10/10 | 148.146 → 42.794 | 70.0% | 10/10 |

收尾修正前也完成过一组 1/8 线程各十对测量，原始结果另存 `intermediate-performance.json.gz`。该版八线程四层重入为 8/10 对更快，另外两对分别慢约 0.4% 和 1.5%；修正后重新测量全部十对，不合并两个制品的数据，也不丢弃前一组结果。

单线程标量 worker 的进程中位数范围为 2.617–76.310 µs，波动依然明显。全部逐对比值与每进程结果保存在 `summary.json`；包括变慢的配对，不筛选数据。

## Measurement boundaries

每组十对，交替 AB/BA，每次新建 VM；固定八批预热、三十批测量，并逐对逐线程校验 checksum。表格是批均摊调用耗时的汇总，**不是单次调用 P99，也不是裸域切换耗时**。八线程各类工作异步推进，是混合竞争负载。两侧使用相同补丁 runtime、实验信号层和 JNI 封送实现，没有绑核，也没有控制温度、DVFS 或后台负载。

不能将 worker 的波动解释为固定桥损耗，也不能推导 CPU/能耗、游戏 FPS 或所有模组的收益。没有普通 bionic JNI 直接基线；本轮仅比较同一夹具中的两种交接方式。

## Boundaries of the recorded runs

- 本轮真实 VM 为固定 Android JRE 17；桌面 OpenJDK 17 是补充机制验证。未验证该后端的 ART、其他 JRE 制品、真实模组或 ClassLoader GC 自动卸载。
- 使用公开 loader API；未覆盖 JRE 私有 `System.load` 钩子的集成。
- 不支持双域独立信号注册/掩码、任意 Guest 自定义处理器、跨域 longjmp/unwind。Guest-only 线程收到必须转交 Host 的信号会终止隔离进程。
- 信号代次入口和登记页保留到进程退出；未实现通用回收。Guest owner 登记前窗口、原始信号系统调用绕过、完整 runtime 卸载仍不在支持范围。
- 本报告对应的生产后端默认 worker，实验 hooks 由 `GW_EXPERIMENT_DOMAIN_JNI` 启用。现行契约见 [JNI runtime 接入](../../../../docs/zh/RUNTIME.md)。

## Evidence

`summary.json` 汇总最终测试、逐对比值和证据 SHA256。`arm64-{1,8}-threads.json.gz` 保留逐批样本、checksum 和兼容性矩阵；`raw-logs.json.gz` 保留对应 stdout/stderr。`build-manifest.json` 记录原生构建输入和制品哈希，含构建机器路径，不是安装目录约定。`application.txt` 和 `application.json` 记录最终 APK 的应用身份验证，`contracts.json` 记录独立信号及正常后端回归。可用 `gzip.decompress` / `json.loads` 直接重算，统计方法与 [前一轮报告](../README.md) 相同。
