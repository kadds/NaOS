# ADR-0003：NaOS TTY/PTY 用户态架构

- 状态：Accepted
- 日期：2026-08-08（2026-08-16 更新）
- 决策者：NaOS kernel 与 userland 维护者
- 适用范围：TTY/PTY native capability ABI、`ttyd`、`consoled`、内核终端机制和 mlibc POSIX compatibility layer
- 相关 ADR：[Capability Handle 与异步 Invocation IPC](OBJECT_CALL_ADR.md)、[TTY/PTY Native Mapping](PTY_ADR.md)

## 背景

NaOS 的终端同时服务于两个不同场景：用户态程序需要 POSIX TTY/PTY 语义，内核处于启动早期、用户态服务失效或发生
panic 时仍需要输出诊断信息。把这两种生命周期和状态模型放在同一个内核 TTY 实现中，会让 line discipline、PTY
队列、终端渲染和用户态兼容层互相耦合，也会让启动和 panic 路径依赖用户态服务。

本决策把 POSIX TTY/PTY 的策略状态移到用户态，同时保留一条不依赖用户态的 kernel early/emergency terminal。native
接口使用 capability 和 typed protocol；POSIX fd、`termios`、`poll`、`ioctl` 等兼容语义由 mlibc 提供。

## 决策

### 职责边界

| 组件 | 负责内容 | 不负责内容 |
| --- | --- | --- |
| kernel early/emergency terminal | 启动日志、kernel log、panic 输出、serial/framebuffer backend、内置 8×16 字体 | POSIX `termios`、line discipline、PTY 编号和用户态 job-control 策略 |
| `ttyd` | PTY pair、master/slave binding、line discipline、`termios`、`winsize`、输入/输出队列、readiness、hangup、PTY 生命周期 | ANSI/VT 解析、字体排版、像素渲染、网络协议 |
| `consoled` | 本地图形 console 的 PTY master、libvterm screen/cell 状态、固定 bitmap glyph 渲染、键盘事件编码、`/dev/fb0` 输出 | PTY policy、line discipline、kernel session/process-group 权限判定 |
| kernel terminal mechanisms | terminal identity、session/foreground process group 关联、受限 job-control action、硬件 input event 发布 | 字节队列、`termios` 状态和用户态终端内容 |
| mlibc | POSIX fd、fd flags、blocking/non-blocking 语义和 POSIX 命名映射 | native capability 的 ownership、scope、rights 和 invocation 生命周期 |

`ttyd` 是唯一的用户态 PTY 状态所有者。master/slave 是终端领域的两个角色，不等同于一条 raw channel 的两个 endpoint；
两者由 `ttyd` 内部的 PTY backing object 关联。

### Native protocol

系统协议定义在 `idl/system/`，由 `idl/naoidl.py` 生成 typed bindings 和 wire codec。终端的 data、control、readiness
和 binding administration 方法属于同一个 per-side protocol endpoint，并按角色授予权限：

| Protocol / view | 所有者 | 作用 |
| --- | --- | --- |
| `TerminalManager` | `ttyd` | 创建 PTY、打开 master/slave、打开本地 console、签发 binding |
| `TerminalMaster` | `ttyd` | master 读写、PTY 管理、属性、窗口大小、flush、readiness |
| `TerminalSlave` | `ttyd` | slave 读写、属性、窗口大小、flush、readiness |
| `TerminalJobControl` | kernel view | attach/detach、session 与 foreground process group、I/O 检查 |
| `TerminalDriverControl` | kernel view，仅 `ttyd` | foreground signal、窗口变化、hangup、revoke |
| `TerminalDriverFactory` | kernel view，仅受信任创建路径 | 创建和校验 terminal identity |
| `InputEventSource` | kernel device capability | 发布有界的键盘事件 channel |

单个 master binding 和单个 slave binding 各自使用一条 typed connection。endpoint 是进程本地、opaque、按 MOVE 规则转移的
capability；可复制的 job-control view 仍受 scope 和 rights 限制。终端字节不嵌入 control message。

首选数据面是有界的 `TerminalMaster.read/write` 和 `TerminalSlave.read/write` message，单个字节块不超过 64 KiB。
队列满时产生 short write、等待或 `EAGAIN`/`WOULD_BLOCK`，不能静默丢弃字节。只有在测量确认 RPC 数据面成为瓶颈且拥有独立
协议决策时，才允许引入共享 ring；本 ADR 不把共享 ring 作为默认 ABI。

### `ttyd` 状态与调度

`ttyd` 的 PTY backing object 持有：

- canonical/raw line discipline 和输入转换；
- `termios`、`winsize`、EOF、flow control、echo 和 signal-generating control characters；
- master/slave 两侧的有界 byte queue；
- PTY 编号、generation、lock/unlock、binding 数量和 hangup 状态；
- pending read/write/watch responder，以及每个 binding 的请求额度。

服务 dispatcher 不在等待字节时阻塞整个 endpoint。阻塞 read/watch 保留 responder，状态或 generation 改变时重新检查；
control、write 和取消仍能被调度。服务 crash 或 endpoint close 会释放 pending responder，旧 endpoint 不会自动连接到新的
服务实例。

所有 PTY 数量、endpoint 数量、队列容量、pending invocation 数量和单连接请求数都有硬上限。达到上限时返回明确错误或
施加背压，不能通过无界分配恢复吞吐。

### `consoled` 与 framebuffer

本地 console 的数据流为：

```text
InputEventSource -> keymap -> libvterm keyboard encoding -> TerminalMaster.write -> ttyd
ttyd -> TerminalMaster.read -> libvterm screen/damage -> fixed 8x16 glyphs -> mmap(/dev/fb0)
```

`consoled` 使用 libvterm 保存 VT screen/grid、cursor 和 damage 状态，使用内嵌固定 8×16 bitmap glyph 绘制 ASCII
`U+0020..U+007E`；其余 codepoint 使用固定 replacement glyph。首期不依赖 FreeType、HarfBuzz、fontconfig 或外部字体
加载。

runtime `/dev/fb0` 只有一个 user writer。`consoled` 通过启动时收到的
`NA_BOOTSTRAP_CAPABILITY_CONSOLE_FRONTEND` capability 获得 framebuffer writer 权限；普通终端子进程不会继承该 capability。
kernel normal log 不与 `consoled` 并发写 framebuffer；panic 路径先停止其他
CPU 和 user execution，再由 kernel emergency terminal 直接接管 framebuffer。kernel emergency terminal 不等待
`consoled`，也不依赖 libvterm、allocator、VFS 或用户态服务。

### mlibc 映射

| POSIX 操作 | Native 映射 |
| --- | --- |
| `open("/dev/ptmx")`、`open("/dev/pts/N")` | `TerminalManager` 的 PTY open method |
| `read`、`write` | 对应 `TerminalMaster` 或 `TerminalSlave` 的 data method |
| `tcgetattr`、`tcsetattr` | `get/set_attributes` |
| `ioctl(TIOCGWINSZ)`、`ioctl(TIOCSWINSZ)` | `get/set_winsize` |
| `tcflush` | `flush` |
| `tcgetpgrp`、`tcsetpgrp`、`TIOCSCTTY`、`TIOCNOTTY`、`TIOCGSID` | `TerminalJobControl` view |
| `poll`、`select` | readiness query 和 generation-based watch |
| `SIGTTIN`、`SIGTTOU` 检查 | `TerminalJobControl.check_io` |
| Ctrl-C、Ctrl-\\、Ctrl-Z | `ttyd` line discipline -> `TerminalDriverControl` |

POSIX fd 编号、`O_NONBLOCK`、`O_APPEND`、`FD_CLOEXEC` 和 job-control sidecar 只存在于 mlibc。compatibility layer 不得
改变 native handle 的 capability ownership、scope、rights 或 invocation 生命周期。

## 不属于本 ADR 的范围

- 通用 compositor/window system、keyboard driver、framebuffer driver 和整个 VFS 的重构；`consoled` 使用现有 input event
  与 framebuffer ABI。
- FreeType、HarfBuzz、fontconfig、动态字体 fallback、ligature 和复杂文字 shaping。
- `sshd`、SSH 认证/加密、网络监听、远程 session adapter 和网络运行时。
- 以 shared ring 替换全部 terminal data RPC。
- 完整 POSIX termios 扩展、packet mode、Unix98 权限模型和 serial modem ioctl。
- service restart 后旧 fd 的透明续接。
- 通过任意 PID 和 signal number 伪造调用者身份取得通用进程控制能力。

## 不变量与失败语义

1. 同一个 logical terminal 在任意时刻只有一个状态所有者：kernel emergency terminal 或 `ttyd`，不能双写或双读。
2. channel admission、terminal byte queue 和 framebuffer/backend queue 都受有界容量约束。
3. 输入或输出方向关闭后，已缓存字节按 drain 规则可读；队列耗尽后返回 EOF、hangup 或确定的 errno。
4. peer close、cancel、deadline、service crash 和 revoke 都会让对应 invocation 得到确定结果；资源 disposition 按事务规则
   提交或回滚。
5. session、process group 和 signal 操作以 kernel object 与 current caller 为依据，不信任 `ttyd` 传入的数字 PID。
6. early/emergency terminal 使用预分配或静态状态以及内置字体，不能调用用户态 TTY 服务。
7. runtime framebuffer 只有一个 user writer；panic 接管时不等待用户态协作。

## 备选方案

### 继续把完整 TTY 保留在内核

实现改动较少，但会让终端策略、PTY 资源管理和用户态兼容语义继续占用内核边界，也无法自然复用 typed capability protocol。
该方案不采用。

### 为 TTY 增加专用 syscall

可以绕过 service endpoint 的建模问题，但会形成与 channel、capability transfer 和 invocation 生命周期割裂的第二套 IPC ABI。
该方案不采用。

### 让 `ttyd` 直接渲染 framebuffer

这会把 byte/line discipline 服务与 VT emulation、字体和像素设备绑定，阻止同一 PTY 服务被其他 master frontend 使用。
该方案不采用。

### 默认使用 shared ring

shared ring 适合已经被测量证明的高吞吐数据面，但会增加映射、所有权和生命周期约束。首版先使用已有有界 typed RPC，以保持
协议和失败语义一致。

## 后果

正面结果：

- TTY/PTY 策略状态和渲染状态从 kernel 移出，内核只保留必要机制；
- panic、启动日志和用户态 terminal service 具有独立生命周期；
- master frontend 可以复用同一 PTY 协议，renderer 不会污染 line discipline；
- mlibc 的 POSIX compatibility mapping 与 native capability ABI 清晰分层；
- 有界队列、generation readiness 和明确的 endpoint close 语义能够覆盖阻塞、背压和 hangup。

代价：

- 一次终端读写经过 typed invocation，服务端需要维护 pending responder 和额度；
- `ttyd`、`consoled` 和 kernel mechanisms 之间存在多个 capability 边界，启动编排和故障诊断更复杂；
- framebuffer 单写者和 panic 接管规则需要由设备能力与服务生命周期共同保证；
- POSIX 扩展必须分别在 mlibc、`ttyd` 和 kernel job-control view 中保持一致。

## 当前实现证据

当前源码中的决策落点为：

- `naos/src/usr/ttyd/terminal_core.hpp`、`terminal_core.cc`：用户态 line discipline、termios、winsize、队列、readiness
  generation、EOF 和 hangup 状态机；
- `naos/src/usr/ttyd/main.cc`：`TerminalManager`、master/slave binding、pending responder、额度和 kernel driver view
  交互；
- `naos/src/usr/consoled/main.cc`、`vga_font.hpp`：本地 PTY master、libvterm、键盘事件和 framebuffer renderer；
- `idl/system/terminal_*.naidl`：native terminal protocol schema；
- `naos/includes/kernel/dev/tty/` 与 kernel invocation/terminal 代码：terminal identity、job-control mechanism 和
  early/emergency terminal；
- `naos/libc/mlibc/sysdeps/naos/generic/generic.cpp`：POSIX fd 与 terminal protocol 的 compatibility mapping。

验证记录：

- `cmake --build build --target naos_unit_tests consoled init -j2` 通过；
- `ctest --test-dir build --output-on-failure` 通过，4 个测试全部通过；
- `ninja -C naos/libc/mlibc/build -j2` 通过；
- QEMU ISO 启动达到 `init: user shell started`，未观察到 kernel fault。
