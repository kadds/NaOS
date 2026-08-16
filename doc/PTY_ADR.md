# ADR-0002：NaOS TTY/PTY Native Mapping

- 状态：Accepted（用户态 backend 已默认启用）
- 日期：2026-08-08（2026-08-09 更新）
- 决策者：NaOS kernel 与 userland 维护者
- 适用范围：NaOS TTY/PTY 用户态迁移、native capability ABI 和 mlibc POSIX
  compatibility layer
- 相关 ADR：[Capability Handle 与异步 Invocation IPC](OBJECT_CALL_ADR.md)、
  [TTY/PTY 用户态架构](TTY_PTY_USERSPACE_ADR.md)

TTY/PTY 是 capability-backed 协议，但 **line discipline 与 stream state 由
用户态 `ttyd` 持有**。内核只保留 terminal-scoped 的
session/foreground-group 机制（`terminal_identity`）与 early/emergency
console。字节走 `TerminalMaster`/`TerminalSlave` 协议，不嵌入 control
message。

## Capability surfaces

| Capability | Scope | 归属 | Examples |
| --- | --- | --- | --- |
| `TerminalManager` | `NA_SCOPE_TERMINAL_MANAGER` | ttyd 服务端 | create_pty、open_pty_slave、open_console、open_controlling |
| `TerminalMaster` / `TerminalSlave` | 对应 scope | ttyd 服务端 | read/write、termios、winsize、flush、query/watch、clone_binding |
| `TerminalJobControl` | `NA_SCOPE_TERMINAL_JOB_CONTROL` | kernel view | check_io、attach/detach、get/set_pgrp、get_sid、query |
| `TerminalDriverControl` | `NA_SCOPE_TERMINAL_DRIVER_CONTROL` | kernel view，仅 ttyd | raise_foreground、notify_winsize_changed、hangup、revoke |
| `TerminalDriverFactory` | `NA_SCOPE_TERMINAL_DRIVER_FACTORY` | kernel view，仅 ttyd | create identity、validate_locator |
| `InputEventSource` | `NA_SCOPE_INPUT_EVENT_SOURCE` | kernel device | subscribe -> 有界 KeyEvent channel |

handle 是进程本地且 opaque。KernelView 只允许按其 metadata 明确授予的
权限复制（`NA_RIGHT_DUPLICATE`）。

## mlibc compatibility mapping

| POSIX operation | Native operation |
| --- | --- |
| `open("/dev/ptmx")` / `open("/dev/pts/N")` | mlibc path bridge -> `TerminalManager` |
| `tcgetattr` / `tcsetattr` | `TerminalMaster/Slave.get/set_attributes` |
| `ioctl(TIOCGWINSZ)` / `ioctl(TIOCSWINSZ)` | get/set_winsize（set 后 ttyd 经 driver_control 发 SIGWINCH） |
| `tcflush` | `TerminalMaster/Slave.flush` |
| `tcgetpgrp` / `tcsetpgrp` | `TerminalJobControl.get_pgrp/set_pgrp` |
| `TIOCSCTTY` / `TIOCNOTTY` / `TIOCGSID` | `TerminalJobControl.attach/detach/get_sid` |
| read/write 的 SIGTTIN/SIGTTOU 检查 | `TerminalJobControl.check_io` |
| Ctrl-C / Ctrl-\\ / Ctrl-Z | ttyd line discipline -> `TerminalDriverControl.raise_foreground` |
| `poll`/`select` | `TerminalMaster/Slave.query` + `watch`（generation-based） |
| 键盘输入 | kernel `InputEventSource` -> `consoled` keymap -> master write |

mlibc 拥有 POSIX `fd` 编号、`O_NONBLOCK`、`O_APPEND`、`FD_CLOEXEC` 和
terminal `job_control` sidecar。非阻塞 POSIX 调用只在 mlibc 层保持语义，
不改变 native handle 的阻塞模式。

## Lifecycle and failure rules

- 关闭 fd 释放 mlibc binding（含 job_control sidecar）；不会取消已受理的
  invocation。
- terminal hangup 由 ttyd 队列语义表达（master/slave hung up -> readiness
  hangup 位）；peer close 由 endpoint `PEER_CLOSED` 表达。
- `TerminalDriverControl.revoke/hangup` 由内核 detach session 并向前台组
  发 SIGHUP/SIGWINCH。
- process-group/session 检查发生在 `TerminalJobControl.check_io` 边界。
- `ttyd` crash 时所有 pending read/watch/responder 随 endpoint close 释放；
  `/dev/fb0` writer 随进程 VM teardown 释放。
- PTY endpoint ownership 遵循 MOVE-only capability 规则；job_control 是
  可 DUPLICATE 的 KernelView。
