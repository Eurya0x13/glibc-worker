# GNU 信号目标的创建生命周期

[English](../en/THREAD_LIFECYCLE.md) | [简体中文](../zh/THREAD_LIFECYCLE.md)

匹配 libc 的可选接口 `__gw_domain_thread_create_hook` 将信号目标分配接入真实
NPTL 创建流程。必须在串行引导阶段、创建应用线程前注册版本 1 配置。配置包含
精确结构大小和四个必需回调，libc 会复制配置。无效参数返回 `EINVAL`，重复注册
返回 `EBUSY`。接口没有注销操作；回调及其代码必须保持到运行时完成退役。

## 创建阶段

1. `prepare(key, logical_mask, physical_mask, cookie_out)` 在描述符和继承掩码
   初始化后、clone 前执行。分配接收端、预留身份，为父线程和预期子线程分别持有
   生命周期引用。返回正错误码会阻止 clone；此时必须自行清理部分准备结果，后续
   不会执行完成或子线程回调。原生 `pthread_create` 仍将 `ENOMEM` 转为 `EAGAIN`。
2. `complete(cookie, tid, error)` 在准备成功后恰好执行一次，位于原生 clone 和
   亲和性、调度设置完成后。正 TID 来自 clone 的真实返回值，即使后续设置失败也
   保留；零表示 clone 没有创建线程。成功完成必须在 `pthread_create` 返回前启用
   身份，即使子线程尚未开始执行。
3. `enter(cookie)` 在真实子线程第一次恢复原生掩码前执行。原有
   `__gw_domain_thread_hook` 的进入回调先执行，因此外层内核路由已经可以找到 GNU
   TCB。此处完成入口状态并启用同一目标；相同 TID 的重复启用是幂等操作。
4. `leave(cookie)` 在子线程用户清理、TLS 析构后执行，早于原有生命周期回调撤销
   外层路由。撤销发送、清除私有待决队列并释放子线程引用。普通返回、
   `pthread_exit` 和原生取消均经过此路径。

子线程进入、退出都可能早于父线程的完成回调。分离线程的描述符此时可能已经释放
或复用。完成回调必须使用独立持有的 cookie，不能解引用描述符 key。NPTL 在
clone 前保存 cookie，直接传给子线程回调；父线程另有局部副本。原生暂停启动
设置失败会跳过两个子线程回调，因此带错误的完成回调必须同时退役预留的子线程
引用和父线程引用，不能将失败的子线程暴露为可执行目标。

四个回调执行时，可捕获信号均被物理屏蔽；libc 保留每个回调边界的 errno。
这不为整个 `pthread_create` 增加 errno 保留保证：原生 clone 回退即使最终创建
成功，也可能改变 errno。回调必须维持
临界掩码，不得执行应用代码、非局部跳转或等待接收线程。测试会故意停顿回调以
暴露顺序竞态；生产回调必须采用非阻塞发布。接口不为 GNU 线程制造 Bionic 状态，
也不改变公开 pthread 描述符的规则。

## Core 预备目标

`gw_signal_target_prepare` 预留稳定接收端和 key，TID 为零，附带一个创建者 pin。
通过 `retain` 派生预期子线程的引用。预备目标不能消费消息或接收通知，包括共享
队列准入、掩码变化和通知重试。共享消息继续保持待决。

`gw_signal_target_activate(target, tid)` 要求正 TID，且不分配内存。启用和检查
既有待决消息在同一事务中完成。重复相同 TID 成功，且不重复初始通知；不同 TID
返回 `EALREADY`。关闭后返回 `ESRCH`，即使原描述符 key 已有新的目标身份。初始
唤醒失败仍保留已经提交的启用状态，通过 `gw_signal_targets_error` 暴露。

普通 `open` 仍是单个注册表事务，不暴露中间预备阶段。关闭、释放保留原有的续体
契约。调用者必须在启用期间持有目标；创建者 pin 不能证明已保存的信号帧或 TCB
存储可以回收。

## 验证与范围

`tests/guest-async/create-control.c` 将 owning libc 回调接入真实的 core 预备目标。
覆盖 130 个子线程、父子两种顺序、64 次分离线程早退、原生取消清理、子线程准入
前的信号发布、显式 `pthread_exit`、TSD 析构、配置复制、继承快照及回调边界的
errno。独立失败用例拒绝准备、在 clone 成功
后制造亲和性设置失败，以及通过仅作用于调用线程的 seccomp 过滤器强制 clone
失败。失败设置不得进入用户代码或子线程回调，所有目标引用必须退役。
独立的 `--child-only` 对照故意省略父线程准入，必须对尚存活、但暂停在自身准入
之前的子线程复现 `ESRCH`；完整路径必须接受相同发送，并消费原始消息。
`tests/guest_async.py` 使用选定 libc 在 Linux 或 Android ARM64 上运行。注册表
测试另覆盖并发启用、预备期间共享消息保留、key 复用后的迟到启用，以及唤醒失败
重试。

这些对照建立了创建顺序和存储生命周期保证。已安装 JNI 服务仍需将回调接入公开
Guest 独立处置及异步入口；该对照不能代替这项兼容性验收。原生描述符所有权和
启动失败处理见
[NPTL 创建实现](https://github.com/bminor/glibc/blob/glibc-2.44/nptl/pthread_create.c)。
