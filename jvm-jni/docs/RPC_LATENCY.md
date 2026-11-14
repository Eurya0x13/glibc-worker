# RPC 等待策略的验证

等待原语与分段接口见 [core 等待契约](../../core/docs/WAITING.md)。本项目只负责真实 VM 集成和性能对照，不在这里维护第二份事件实现。

## 配置与顺序

默认 `spin=20000ns`、`wake_filter=0`、`ready_poll=0`、`profile=0`。Host、Guest 和嵌入入口必须使用相同配置；每个候选 APK 应单独构建并保留散列。不要在同一构建目录并发生成 A/B。

先执行 core 的 ASan/UBSan 事件、交接和响应轮询测试，再运行真实设备的严格 smoke/coverage，最后才计时。

```sh
python3 jvm-jni/tests/coverage/compare_waiting.py --device SERIAL \
  --baseline /path/to/control.apk --candidate /path/to/candidate.apk \
  --benchmark-jit --output jvm-jni/build/test-results/waiting-comparison
```

默认每侧十轮、成对平衡顺序，每轮新进程。严格模式使用解释器和 CheckJNI；普通计时显式启用 benchmark JIT。两种模式不可合并比较。

## 证据要求

scalar echo 和 Guest→Java 回调测量完整往返，不能当作单向时间、数组带宽或游戏帧率。保存功能检查、原始延迟、CPU、空闲等待、热状态和完整生命周期。失败或未结束轮次不得纳入完整对照。

配对诊断分解 outbound/execute/resume；毫秒长尾应结合相同 TID/时间窗的调度 trace，区分实际执行、阻塞和已就绪等待 CPU。有限 outlier 不能代表全部调用分布。profile 增加采样时钟与内存开销，诊断运行不替代普通构建 A/B。

只有真实路径持续改善且兼容性无回归，才考虑更改默认值。减少 futex 次数或微基准中位数下降本身不构成产品收益。
