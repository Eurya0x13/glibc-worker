# 托管栈帧与原生 continuation

[English](../en/NONLOCAL_VM.md) | [简体中文](../zh/NONLOCAL_VM.md)

通过原生转移跳过 Java/VM 帧不属于 glibc-worker 的恢复契约。恢复原生寄存器不会执行
Java 栈清理，包括同步状态。项目负责跨 libc 域的有效原生 continuation 恢复及桥资源
协调，不增加 Java 栈展开，也不为这种跳转要求定制 JRE。

## 可复现对照

显式选择包含编译器、头文件和 `lib/server/libjvm.so` 的 Linux JDK：

```sh
python3 jvm-jni/tests/nonlocal-vm/run.py --jdk /path/to/reviewed-jdk17
```

复现程序不装载 glibc-worker 或第二套 libc，只嵌入选定 HotSpot 并注册两个普通 native
方法。Java 进入 monitor，调用原生代码，并包含 `finally`。原生代码分别执行正常返回、
抛 Java 异常后返回，或以 `siglongjmp`/`setcontext` 直接转移到仍有效的原生检查点。

独立附着的等待线程随后尝试获取同一 monitor；另一个附着线程通过 JVMTI 检查其实际
线程状态。因此失败观测是 `BLOCKED_ON_MONITOR_ENTER`，不是根据一次睡眠超时推测。
解释器和默认执行均在独立子进程运行。被放弃的 VM 通过销毁进程处理，绝不报告为
VM 成功关闭或桥兼容性通过。

选定 JDK17 中，普通返回和 Java 异常对照均执行 `finally`、释放 monitor、让等待线程
继续并销毁 VM；直接原生转移虽然抵达原生目标，却跳过 `finally`，使等待线程阻塞在
monitor。结构化结果和散列写入 `jvm-jni/build/tests/nonlocal-vm/`。这证明存在独立于
双 libc 桥的托管帧清理问题。这是边界证据，不是桥故障、原生恢复阻塞项，也不能证明
HotSpot 有缺陷。尚未在加载桥后运行同一个托管帧复现；已安装桥的测试覆盖的是不同的
纯原生转移。

## 项目责任

有效的原生跨域恢复仍是首版要求，包括嵌套转移及桥持有的 JNI 资源清理。
不能仅因跨越 libc 域，就对原本有效的原生目标改为隔离退出。被放弃的后缀不能包含
Java/VM 帧；后缀之外的 Java 调用者必须在恢复后仍然可用。

正常 JNI 回调、原生返回、Java 异常传播、引用、通过 JNI 操作 monitor，以及受支持
路径上的 VM 线程状态，仍属于兼容性责任。桥不能破坏这些路径。原生跳转不是 Java
异常；桥不使它透明执行被跳过 Java 帧中的 `finally` 或释放其中的 monitor。

破坏性原生跳转和可恢复原生上下文仍需不同的生命周期协议。不支持放弃托管帧，不会
取消原生 `siglongjmp`/`setcontext` 的实现要求，也不代表这些内容已经完成。

HotSpot 的 [`JavaCallWrapper`](https://github.com/openjdk/jdk17u/blob/master/src/hotspot/share/runtime/javaCalls.cpp)
正常返回时协调 frame anchor、句柄和线程状态切换。
[JNI 异常契约](https://docs.oracle.com/en/java/javase/17/docs/specs/jni/design.html#exceptions-and-error-codes)
在原生调用返回时传播待处理 Java 异常。两者都没有提供通用的跨托管帧 `setcontext`
恢复接口。
