# ADR-0002：NaOS TTY/PTY Native Mapping

- 状态：Accepted（实施中）
- 日期：2026-08-08
- 决策者：NaOS kernel 与 userland 维护者
- 适用范围：NaOS TTY/PTY kernel mechanism、native capability ABI 和 mlibc POSIX compatibility layer
- 相关 ADR：[Capability Handle 与异步 Invocation IPC](OBJECT_CALL_ADR.md)

TTY and PTY are capability-backed protocols. The kernel owns the terminal
mechanism and stream state, but callers access control operations through
typed KernelView capabilities. Terminal bytes use Stream operations and are
not embedded in control messages.

## Capability surfaces

| Capability | Scope | Examples |
| --- | --- | --- |
| Stream | `NA_SCOPE_STREAM` | read, write, readiness and peer-close signals |
| TTY control | `NA_SCOPE_TTY_CONTROL` | attributes, window size, flush, process group and session state |
| PTY administration | `NA_SCOPE_PTY_ADMIN` | number, unlock and endpoint administration |

The handles are process-local and opaque. TTY control acquisition creates a
separate scoped view; it does not relabel the Stream handle. Control views are
not duplicated by the generic object layer unless their metadata explicitly
grants that operation.

## mlibc compatibility mapping

The POSIX API is implemented in mlibc and translates to the native methods:

| POSIX operation | Native operation |
| --- | --- |
| `tcgetattr` / `tcsetattr` | get/set terminal attributes |
| `ioctl(TIOCGWINSZ)` / `ioctl(TIOCSWINSZ)` | get/set window size |
| `tcflush` | flush input/output queues |
| `tcgetpgrp` / `tcsetpgrp` | get/set foreground process group |
| `getsid`-style terminal queries | get session ID |
| `TIOCSCTTY` / detach | attach/detach session terminal |
| PTY number/unlock operations | PTY admin methods |
| `poll`/`select` | capability signal wait |

mlibc owns POSIX `fd` numbers, `O_NONBLOCK`, `O_APPEND`, and `FD_CLOEXEC`.
Those policies are not stored in kernel capability metadata. A nonblocking
POSIX call returns after inspecting signals and never changes the native
handle's blocking mode.

## Lifecycle and failure rules

- Closing the fd releases the mlibc binding; it does not cancel an already
  admitted invocation.
- TTY hangup is reported as `PEER_CLOSED`; revocation is reported as
  `OBJECT_REVOKED`.
- A failed output copy leaves the invocation result intact, so retrying the
  result take cannot execute the control method again.
- Process-group and session checks happen at the typed control boundary.
- Process session/process-group identity and mutation use the scoped `Process` KernelView job-control methods; TTY control retains only terminal ownership and foreground-group operations.
- PTY endpoint ownership follows the normal MOVE-only capability rules.

This document describes the POSIX compatibility API exposed by mlibc, not a
second native ABI.
