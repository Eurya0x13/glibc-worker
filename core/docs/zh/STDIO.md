# Host stdio 与延迟信号描述符

[English](../en/STDIO.md) | [简体中文](../zh/STDIO.md)

显式延迟故障信道使用私有内核通知保留原始 Host 信号元数据。Host FILE 读取这个
signalfd 时，应在字节进入原生缓冲区前还原通知。这仅针对信号描述符，与普通
Host/Guest 文件操作及 JNI 数组无关。

适配器保留所属 libc 的真实 FILE 对象、锁、缓冲、方向、seek/reopen 行为及描述符
所有权。不创建 cookie stream、额外描述符、替代 FILE 分配或逐流记录。
`fileno`、无锁字节读取、宽字符操作、`freopen` 和 `fclose` 均保留原生实现。
过小的无缓冲 signalfd 读取、非阻塞 `EAGAIN` 等错误仍按原生规则返回。

## Host 后端

| Host | 接入方式 |
| --- | --- |
| Android bionic LP64 | 已验证制品的原生 stdio 已经进入公开 read 适配。启用信道前，用私有管道及 fread 临时观测缓存中的 read 后端，确认实际路径，随后还原缓存。不访问私有 FILE 字段，不替换工厂函数，不写 libc 数据，也不在正常读取中保留探测分支。 |
| Linux GNU libc LP64 | 原生 FILE 读取可能绕过公开 read。先校验公开的字节/宽字符读取表尺寸、原始回调、相同 libc 归属及完整受保护的 GNU RELRO 页，再在串行启动边界仅替换现有读取回调。普通文件保留原生 I/O；虚拟 signalfd 使用短出队事务，保留原生取消策略和 FILE 记账。 |

GNU vtable 的身份和校验保持不变，不允许外部伪造表，不修改指令。数据页暂时可写，
在应用恢复执行前还原为只读。布局不符返回 `ENOTSUP`，写权限被拒绝返回原生错误。
回滚失败属于终止性错误，返回 `EIO` 并保留所有回调代码，由宿主丢弃进程；组件不会
自动杀应用。

两种后端均通过正常读取后端覆盖已有原生 FILE。FILE 之外的隐藏读取路径、原始系统
调用及绕过显式启动契约的接入不属于此适配范围。`signal_stdio_profile()` 标识当前
后端，不代表任意 Host libc 或 ART 制品已兼容。

## 生命周期

`signal_router_defer_faults()` 检查私有信号编号和描述符清单后激活适配器。
启用和还原时，Host stdio 用户与描述符所有者必须静止；应在创建 VM 或允许生产者
运行前完成。激活前失败不能继续允许虚拟信号描述符进入使用。
普通 SDK 启动不会隐式启用此延迟信道。

`signal_router_restore()` 要求所有虚拟描述符和延迟接收者退出，再还原 GNU 原生
读取回调。bionic 无需还原 FILE 回调或表，因为使用的就是已验证的原生 stdio 路径。
临时能力探测结束后，正常读取没有额外探测分支。既有消费操作规则仍适用；该功能
不代表通用保存/恢复上下文生命周期或原生/延迟异步处理器优先级已经完成。
消费接口遵守 [Linux 选择契约](SIGNAL_WAITING.md)。

## 验证

通过跨平台套件运行维护中的原生对照夹具：

```sh
python3 core/tests/signal_deferred.py
python3 core/tests/signal_deferred.py --ndk "$ANDROID_NDK_HOME" --serial "$ANDROID_SERIAL"
```

`signal_stdio_test` 与原生 signalfd 比较完整元数据，再检查部分/无锁读取、`EAGAIN`
及 `clearerr`、无缓冲错误、宽字符方向、真实 fileno/reopen/close 行为、还原前后的
普通文件，以及 16 个并发接收者各自的流。

同一行为夹具也可链接上一轮 `c034346` core SDK。在 Linux GNU libc 2.41 中，
旧版的虚拟读取暴露私有通知，原始信号元数据断言失败。已测 Android bionic 制品上，
旧 SDK 已通过全部断言，说明应保留其原生路径，不能将其描述为新修复的 Android bug。
可选的弱 profile 查询兼容旧 SDK，但不削弱元数据或 FILE 行为断言。

已安装 JNI 的 `--host-fault-mask` 夹具另在真实 Guest 执行期间产生信号，由 Host
通过 `fdopen/fread` 消费，核对原始元数据，再次调用 JNI、重开原生 FILE 并检查描述符
关闭。判定器要求存在正确的 `host-signalfd-stdio` 结果。

相关实现源码：

- [GNU FILE 读取回调](https://github.com/bminor/glibc/blob/glibc-2.41/libio/fileops.c)
- [GNU 宽字符读取](https://github.com/bminor/glibc/blob/glibc-2.41/libio/wfileops.c)
- [GNU 读取表 ABI](https://github.com/bminor/glibc/blob/glibc-2.41/libio/libioP.h)
- [GNU RELRO 保护](https://github.com/bminor/glibc/blob/glibc-2.41/elf/dl-reloc.c)
- [Bionic 原生 stdio](https://github.com/aosp-mirror/platform_bionic/blob/main/libc/stdio/stdio.cpp)
