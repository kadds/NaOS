# ADR：NaOS Kernel Log 体系

- 状态：Accepted
- 日期：2026-08-17
- 范围：NaOS x86-64 kernel、userland `_s_log`、QEMU 开发运行工具
- 相关文档：[`ARCHITECTURE.md`](ARCHITECTURE.md)
- 取代：旧 `trace::*` 输出协议及其同步 sink 组织方式

## 决策摘要

NaOS 使用 `KLOG_*` 作为 kernel 日志 API，以固定边界的结构化 record 作为日志内部格式，并以 retention ring 作为各输出 sink 的共同数据源。日志调用只负责完成等级判断、上下文采集、格式化和入 ring；串口、console、`/var/log/dmesg` 均由独立 worker 输出。early boot、正常运行和 panic 使用不同生命周期，但共享同一套 record 语义。

日志宏统一使用 freelibcxx 的编译期 `{}` format 风格。颜色只作用于日志头部的标识字段，不染色 message 正文；颜色不会写入 record，也不会污染默认的串口文件日志。

## 背景

旧 `trace` 系统存在以下边界问题：

- 一次调用可能拆成多段输出，导致 ring、串口、终端和 dmesg 看到碎片。
- 全局 logger 锁覆盖格式化和同步 I/O，放大 SMP 竞争，并使串口或 VFS 延迟影响日志调用方。
- 输出缺少统一的时间、CPU、PID/TID、等级、module 和 source location。
- dmesg 在普通日志调用路径中触碰 VFS，不适合 early、IRQ、NMI 和 panic 上下文。
- panic 需要在普通 logger、heap、VFS 或某个 sink 已经失效时仍能打印诊断信息。
- 用户态 `_s_log` 把 syscall 实现模块 `io` 暴露为来源，无法直接识别实际进程。

## 决策

### 1. API、module 与格式化

kernel 源文件声明一个固定 module，日志 API 只接受固定 enum，不把目录名或 `__FILE__` 当作 module：

```cpp
enum class module : u8
{
    kernel,
    arch,
    acpi,
    mm,
    task,
    sched,
    ipc,
    fs,
    dev,
    io,
    tty,
    test,
};
```

调用形式如下：

```cpp
KLOG_MODULE(mm);

KLOG_TRACE("run queue length {}", ready);
KLOG_DEBUG("switch {} -> {}", old_tid, new_tid);
KLOG_INFO("Memory available {} bytes", bytes);
KLOG_WARN("allocator watermark is low");
KLOG_ERROR("channel allocation failed: {:x}", status);
KLOG_PANIC("page allocator metadata corrupt at {:p}", entry);
KLOG_RAW("stack trace:\n{}", frame_text);
```

`KLOG_TRACE`、`KLOG_DEBUG`、`KLOG_INFO`、`KLOG_WARN`、`KLOG_ERROR` 和 `KLOG_PANIC` 产生结构化日志；`KLOG_RAW` 用于 stack、寄存器和固件 dump 等多行诊断。`KLOG_RAW` 不添加普通 record 头，也不承诺单行解析。

禁用等级时，宏在格式化和求值 `__VA_ARGS__` 前完成判断。`KLOG_PANIC` 获得 panic owner 后进入 emergency path。

格式化使用 freelibcxx 的 fixed-buffer `format_to()` 和编译期 `format_string<Args...>`。支持转义花括号、顺序字段 `{}`、整数 `{:d}`、`{:x}`、`{:X}`、指针 `{:p}` 和有限宽度的零填充整数。formatter 不使用 heap、exception、locale、RTTI、hosted I/O 或 `std::format`；不支持的格式和参数类型在编译期拒绝。

### 2. Record 与可见文本

每次日志调用先形成一个完整 record，内部至少保留：

```cpp
struct record_header
{
    u64 sequence;
    u64 timestamp;
    u32 cpu_id;
    u32 pid;
    u32 tid;
    u32 line;
    u8 level;
    u8 flags;
    u16 message_length;
    u8 module;
    u8 file_length;
};
```

另外保留调用文件、有限长度 message 和 userland process name。`sequence` 只用于 ring、cursor、gap 和 panic snapshot，默认不显示在人类可读文本中。timestamp 未就绪时使用 `early`，CPU/PID/TID 未就绪时使用 `?`，不探测未初始化的 task 或 timer 对象。message 和 file 有固定上限，超长时截断并设置 `truncated` 标志。

普通文本格式为：

```text
[000000643462] 0-?-? INFO kernel: SMP init
[000001417443] 0-8-0 INFO ttyd: starting
[early] ?-?-? INFO acpi: ACPI tables ready
```

头部中的 `cpu-pid-tid` 直接使用短字段表示，不重复输出 `cpu=`、`pid=`、`tid=`。日志头不显示 `seq=`；内部 sequence 仍然存在并参与丢失检测。普通 message 保持原文，换行和控制字符按规范文本规则转义。

### 3. userland `_s_log`

`_s_log` 的来源名称取当前进程 executable basename，最多保留 12 个字符；来源不再显示 syscall 所在的 `io` module。若调用方 message 已经带有相同的 `name: ` 前缀，logger 去掉重复前缀，因此输出为：

```text
[000002346804] 0-7-0 INFO init: ttyd ready
[000002489249] 0-9-0 INFO consoled: starting
```

内部仍可用 `module::io` 对 userland record 进行过滤，但渲染出的来源名称始终是进程名。

### 4. 生命周期与并发

logger 状态按 `early -> runtime -> panic` 运行：

- early 使用静态 ring，默认 8 KiB，不依赖 heap、VFS、完整时钟、task 或 terminal。溢出覆盖最旧 record，并累计 dropped 计数。
- runtime 使用固定 slot 的 retention ring，默认 32 KiB，可配置为 8 KiB–1 MiB。ring 只追加或覆盖最旧 slot，读取不会消费历史 record。
- panic 由第一个获得 `panic_owner` 的 CPU 处理。其他 panic/NMI 进入无锁 emergency writer，不等待 logger 或 sink 锁。
- 普通日志只在短临界区内写入 slot、发布 committed sequence 并唤醒 worker，不在核心锁内执行串口、terminal 或 VFS I/O。
- 每个 sink 使用独立 cursor 和 worker；worker 复制一个已提交 slot 后释放核心锁，再完成格式化和输出。cursor 发现 sequence/generation 跳跃时产生 gap 信息。
- NMI 不等待锁；成功时使用 `try_emit`，失败时写入每 CPU 静态 emergency slot 并累计 `nmi_dropped`。

日志丢失按 ring 覆盖、filter 丢弃、worker gap、NMI 失败和 dmesg 写失败分类统计。被覆盖的范围通过 gap/dropped 信息呈现，不把无边界字节流交给 sink。

### 5. Sink 与颜色

普通 sink 的配置格式为：

```text
kernel_log_sinker=console:on:info:color,serial:on:debug:nocolor,dmesg:on:info:color
```

支持的 sink 是 `console`、`serial` 和 `dmesg`；每个 sink 有独立开关、最低等级和颜色开关。默认配置为 console/dmesg 彩色、serial 无色。`quiet` 关闭 console，但不影响 retention ring、serial、dmesg 或 emergency path。

颜色策略如下：

- console 和 dmesg 的 `color` 只给 timestamp、`cpu-pid-tid`、level、PID 和 module/process 标识符着色。
- message 正文不被整行染色，终端当前颜色不会被日志正文改写。
- serial 默认 `nocolor`，因此 `run/kernel_out.log` 和普通 `cat` 输出保持纯文本；显式配置 `serial:...:color` 时只给头部字段着色。
- `nocolor` 会移除 message/raw 中已有的 ANSI；颜色从不写入 retention ring，也不发送到默认 emergency serial。

sink 约束：

- `klog_serial` worker 串行写 COM1，对应 QEMU 的 `-serial file:run/kernel_out.log`。
- `klog_console` worker 写 kernel terminal，不与 logger core 锁形成 I/O 依赖。
- `klog_dmesg` worker 在 task context 写 `/var/log/dmesg`，处理历史 drain、短写和失败重试；dmesg 失败不会递归记录，也不会阻塞其他 sink。
- emergency sink 独立使用 COM1 和可用的 debugcon/panic console，不依赖普通 worker、heap 或 VFS。

### 6. Panic 诊断

panic 主路径输出：

- panic 原因和当前上下文；
- 已提交 retention ring 的最近日志以及 snapshot/gap 状态；
- 通用寄存器、MSR、控制寄存器；
- 当前栈和调用栈回溯；
- 异常入口提供的 interrupt register frame（若存在）。

panic/raw 文本绕过普通结构化前缀，避免把寄存器 dump 误识别为普通日志 record。emergency 串口轮询具有固定上限，设备异常时继续输出其余诊断并记录 I/O failure。panic 后 QEMU 保持现场，便于使用 `kernel.dbg` 连接 GDB。

### 7. kernel command line

支持以下设置：

```text
kernel_log_level=debug
kernel_log_sinker=console:on:info:color,serial:on:debug:nocolor,dmesg:on:info:color
kernel_log_buffer_size=32K
kernel_log_early_buffer_size=8K
kernel_log_filter=mm=debug,sched=warning,ipc=info
kernel_log_emergency_serial=on
```

`kernel_log_level` 控制进入 retention ring 的最低等级；module filter 可覆盖全局等级。`kernel_log_buffer_size` 接受 8 KiB–1 MiB，`kernel_log_early_buffer_size` 接受 1–8 KiB；非法值使用确定的默认值。filter 最多 16 项、总长度最多 256 字节，未知 module、等级、重复项和超长值被忽略并报告 warning。panic 不受普通 sink 等级过滤影响。

### 8. QEMU 调试接口

`util/run.py` 统一组装 ISO、磁盘和 UEFI 的调试参数：

```text
--wait-gdb       添加 -S，等待 GDB
--gdb-port PORT  替换默认 TCP 1234 监听端口
--qemu-debug     将 QEMU 内部日志写入 run/qemu.log
--monitor PATH   创建 QEMU monitor Unix socket
--no-reboot      添加 -no-reboot -no-shutdown
```

端口必须在 1–65535 范围内；monitor socket 已存在时拒绝覆盖。默认 QEMU 路径继续使用 `-s` 监听 TCP 1234。

## 后果

正面结果：

- 每次调用拥有完整边界和稳定 metadata，SMP 输出不会发生参数级或字节级交错。
- 普通日志调用与串口、terminal、VFS 解耦，慢 sink 不会阻塞其他 sink。
- early、NMI 和 panic 可以在普通运行时设施不可用时保留诊断信息。
- 用户态日志直接显示进程名，console/dmesg 的颜色可读且不会污染 message 或默认串口文件。
- retention ring 同时服务 dmesg、panic snapshot 和调试读取，worker 不会消费历史记录。

约束与代价：

- 固定 slot、message/file 上限和固定 module enum 限制了单条日志大小及 module 集合。
- 每个 sink 都需要独立 worker、cursor 和失败统计。
- record 的 sequence 是并发与丢失检测依据，但不承诺跨 CPU 的物理发生顺序。
- raw dump 适合诊断，不属于可无歧义解析的单行结构化协议。

## 明确不在本决策中的内容

本架构不引入用户态 syslog daemon、完整 POSIX syslog ABI、磁盘轮转、远程日志协议或网络 broker；不把 QEMU `-d` trace 转换为 NaOS log；不以无锁 MPMC 队列替换当前固定 slot 方案；不包含 TCG record/replay。上述边界不改变本 ADR 已确定的 kernel log 行为。

## 验证记录

已使用真实构建、C++ 测试和 QEMU 启动验证：

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
python3 util/run.py q --iso --no-reboot -c 2
```

结果：C++ 构建成功，CTest 25/25 通过；2 CPU QEMU 启动能够进入 user shell，serial 日志显示 early/runtime 日志、进程名来源、字段颜色配置和无 panic 的调度运行。`run/kernel_out.log` 是默认串口文本输出位置。

架构总览中的日志生命周期、record 字段、raw/structured 区别、command line 参数和 QEMU 调试选项与本 ADR 保持一致。
