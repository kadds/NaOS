# ADR：Rust Native Userland Compiler TLS ABI

- 状态：Accepted；Rust-only 与 C++/mlibc 联合 Phase 3 smoke 已实现并通过本地验收；dynamic TLS 与 Rust `std` 仍不在本阶段
- 日期：2026-08-18
- 决策者：NaOS kernel / Rust native runtime 维护者
- 关联：[Rust Native Userland ADR](RUST_NATIVE_USERLAND_ADR.md)、[总体架构](ARCHITECTURE.md)

## 1. 背景

NaOS Rust userland 是静态、无 dynamic loader、无 mlibc 依赖的 x86-64 ELF。内核 ELF loader
装载 `PT_LOAD` 并把 program header 位置传入 auxv；它不建立 TLS block。内核调度路径保存每个
线程的 `tcb`，在 user entry 和 context switch 时把它写入 FS base；native `clone(entry, arg,
tcb)` 已在 child 执行第一条用户指令前安装传入的 TCB。

此前的 `naos-runtime::ThreadLocal<T>` 是 32 个固定 16-byte slot 的 runtime helper；它不能代替
compiler TLS。当前 Rust-only vertical slice 已通过真实 `#[thread_local]`、唯一 `PT_TLS`、`.tdata`
复制、child isolation 和 QEMU cold boot 验收；fixed-slot API 仍只作为迁移期 smoke helper 保留。

现在决定支持真正的静态 compiler TLS。第一期接口是 nightly `#[thread_local]`；`std::thread_local!`
属于 Rust `std`，不因本 ADR 自动可用。

这不表示 C++/mlibc 不能支持 `thread_local`。mlibc 的 x86-64 `get_current_tcb()` 同样从
`%fs:0` 读取 `Tcb *`，且其 `Tcb` 已对 self pointer、DTV pointer、stack canary（`fs:0x28`）和
cancel bits 固定了偏移。若 Rust 与 mlibc 分别定义 TCB，任一方都会误读另一方的 FS-relative
状态；因此本 ADR 将 TLS/TCB layout 提升为 NaOS 公共 ABI，而不是 Rust 私有实现细节。

## 2. 决策

NaOS 使用 **System V AMD64 TLS Variant II**，只支持静态 executable 中唯一的 `PT_TLS` module：

```text
low address
  [ static TLS image: .tdata copy + .tbss zero ]
                                                   <- TLS block ends at TP
  [ TCB: self pointer at FS:0, runtime metadata ]  <- TP = %fs.base
high address
```

- `FS.base` 等于 Thread Pointer（TP）与 TCB 起始地址；`%fs:0` 是 TCB 的 self pointer。
- `.tdata` 从 ELF `PT_TLS` template 复制，`filesz..memsz` 清零；应用 TLS symbol 的 offset 只由
  linker 与 `PT_TLS` contract 决定，runtime 不使用固定 slot 编号解释它。
- TLS image 和 TP 的对齐由 `PT_TLS.p_align` 与 16-byte TCB alignment 的最大值决定；p_align 必须
  是非零 power of two，且不超过发布时的 `NAOS_TLS_MAX_ALIGN`。runtime 保留 TLS image 的
  ELF-required congruence，不能只把 TCB 随意放在 mapping 尾部。
- `naos_tls_abi_v1` 是 Rust runtime、mlibc CRT 和任何未来 language runtime 共用的
  template/layout/TCB contract。每个进程只能有一个 TLS allocator/initializer；kernel 只保存、
  验证和装载 FS base，不解码 TLS bytes、语言 value、DTV 或 destructor。
- target 已设置 `has-thread-local=true` 与 `singlethread=false`。每次变更仍必须有实际 `PT_TLS`
  smoke；单改 target JSON 不构成 TLS support，smoke 失败必须在发布前修复或回退该开关。

### 2.1 `naos_tls_abi_v1` 公共 TCB 前缀

TP 以下是 static TLS image；TP 及以上是 TCB。以下前缀是 ABI，必须同时由 `naos-runtime` 和
mlibc 初始化、保持并用 C++/Rust `static_assert`/layout test 锁定：

| 相对 `%fs` 偏移 | 字段 | 规则 |
| --- | --- | --- |
| `0x00` | `self_pointer` | 必须等于 TP；Rust 与 mlibc 都通过它取得当前 TCB。 |
| `0x08` / `0x10` | `dtv_size` / `dtv_pointer` | v1 static-only 模式使用确定的单 module 表示；不得为 Rust 与 mlibc 各建一份 DTV。 |
| `0x18` / `0x1c` | `tid` / `did_exit` | 供 native/compat thread lifecycle 共享；不作为 capability identity。 |
| `0x28` | `stack_canary` | 保留 mlibc/GCC x86-64 的固定 ABI offset。 |
| `0x30` | `cancel_bits` | 保留 mlibc thread/cancellation 使用的固定 ABI offset。 |

mlibc 的 `Tcb` 可以在此前缀后追加其 pthread、cleanup 和 `__cxa_thread_atexit` 状态；Rust-only
进程可以只分配公共前缀及 Rust 所需扩展。若一个静态 executable 同时链接 Rust 与 mlibc，必须
通过共同的 TLS provider 分配一份至少容纳 mlibc `Tcb` 的 block，并由 provider 统一初始化前缀。
禁止两个 runtime 各自调用 `set_tcb` 或分别分配 TLS。

## 3. 初始化与线程方案

### 3.1 初始线程

`_start` 到 runtime 的 early path 只能由汇编和经审计、没有 TLS reference 的 Rust 代码组成：

1. 从 initial stack 的 auxv 取得 `AT_PHDR`、`AT_PHENT`、`AT_PHNUM`，遍历 program headers。
2. 接受零或一个 `PT_TLS`；拒绝多个 template、`filesz > memsz`、非 power-of-two alignment、
   template 不在 user `PT_LOAD`、整数溢出或超过 TLS size/alignment hard limit 的 binary。
3. 通过 raw native `memory_map` 分配 TLS image + `naos_tls_abi_v1` TCB。此路径不得使用
   `GlobalAlloc`。
4. 按 Variant II layout 复制 `.tdata`、清零 `.tbss`、写入 TCB self pointer 与 runtime allocation
   metadata，然后调用 native `set_tcb(TP)`。
5. 只有 `set_tcb` 成功后，才可运行 bootstrap、allocator 或应用中可能访问 TLS 的 Rust 代码。

无 `PT_TLS` 的 static executable 仍获得最小 TCB，使 runtime 的 thread bookkeeping 一致；它不
因此获得 dynamic TLS 或 `std` 支持。

mlibc CRT 采用相同的 discovery、copy/zero、alignment 和 `set_tcb` 顺序；它只能在公共前缀
ready 后初始化 mlibc extension 和 C++ 运行时。C++ `thread_local` 的 destructor 注册、执行顺序
及 `__cxa_thread_atexit` 由 mlibc extension 管理，但不能改变 TP 或 static TLS image 的位置。

### 3.2 子线程

`spawn` 从初始线程保存的 immutable TLS template descriptor 创建独立 TLS/TCB：

1. 分配并按同一 layout 初始化 TLS image/TCB，再创建独立 child stack 与 join state。
2. 将 TP 作为第三个参数传给 native `clone`；kernel 在 child first user entry 前设置线程 TCB/FS。
3. child trampoline 只校验当前 TP/`%fs:0`；它不应把“调用 `set_tcb` 后再开始”作为正确性前提。
4. child 发布 result、以 futex 唤醒 joiner，然后走无 TLS reference 的 exit tail。TLS mapping 的
   unmap 与 `_s_exit_thread` 必须相邻且不可返回，或由明确的 reaper 回收；禁止 unmap 后继续执行
   普通 Rust cleanup。

main process 退出时由进程地址空间销毁回收 main-thread TLS。join state 不在即将解除映射的 TLS
block 中，故 joiner 不会保留指向 child TLS 的引用。

### 3.3 受支持和不受支持的 Rust 表面

| 支持 | 不支持 |
| --- | --- |
| static executable 的 nightly `#[thread_local]`、C++/mlibc `thread_local`、static local-exec TLS、native `spawn` 线程隔离 | `std::thread_local!`、`std::thread`、dynamic TLS、`dlopen`/`dylib`、多 module TLS |
| Rust `Copy` 或无 destructor 的 TLS 值；mlibc 管理 C++ TLS destructor | Rust TLS destructor、panic unwind、两个独立 TLS allocator/TCB layout |

`ThreadLocal<T>` fixed-slot API 只在迁移期保留给现有 smoke；它不是 stable compiler-TLS SDK contract，
后续应删除或降为不暴露 TLS layout 的内部测试工具。

## 4. 后果

### 正面

- Rust 编译器可生成真实 TLS relocation，应用不再被 16-byte/32-slot runtime helper 限制。
- kernel 保持 language-neutral：它只处理 address-space ownership 和 FS-base context switch。
- 单 executable、单 static TLS module 避开 DTV 与 dynamic loader，适合当前 NaOS boot/runtime 模型。

### 代价与限制

- early startup、allocation、template copy 与 exit tail 必须严格避免 TLS reference；这要求 assembly
  边界和 `objdump`/ELF 检查。
- 每个线程都复制完整 static TLS image；大 TLS 会提高 spawn 的内存和时间成本，因此 TLS size 有
  明确 hard limit。
- mlibc 必须改为遵守 `naos_tls_abi_v1`；这会把它现有 x86-64 TCB prefix 变成受版本管理的公共
  契约。Rust-only 与 mlibc-only 程序可各自拥有 extension；混合 executable 必须接入共同 provider。
- 这不是 Rust `std` port；依赖 `std::thread_local!` 的 crate 仍不可直接使用。

## 5. 验收与回滚门槛

在 target JSON 已启用 compiler TLS 后必须持续通过：

1. Rust TLS smoke 使用 `#![feature(thread_local)]` 与至少一个 `#[thread_local] static`；最终 ELF
   有唯一 `PT_TLS`、无 `PT_INTERP`/`NEEDED`，且没有 mlibc/crt link input。
2. 主线程写入值 A、child 写入值 B；join 后主线程仍读到 A。重复 spawn/join 不能复用旧 TLS bytes。
3. 负向测试覆盖 multiple `PT_TLS`、坏 `p_align`、`filesz > memsz`、overflow、size-limit、map failure、
   `set_tcb` failure、clone failure、child early exit 与 join/drop race。
4. QEMU cold boot 连续运行 TLS smoke；serial log 无 kernel fault，thread exit 后无残留 wait state
   或 user mapping leak。
5. C++/mlibc TLS smoke 与 Rust TLS smoke 分别和联合运行；当前联合 ELF 由 mlibc 提供 TCB，
   链入 Rust `#[thread_local]` probe，验证公共 TCB prefix offset、主/child isolation、C++
   destructor、join/detach 回收与 Rust 无 destructor 的退出顺序。
6. 通过 C++ kernel tests 和 Rust tests 验证行为；不得用 Python 源码扫描代替 ABI/TLS 测试。

任何一项失败都是发布阻断项：必须修复 implementation，或将 target 回退为
`has-thread-local=false` 与 `singlethread=true`，仅保留已经可验证的 fixed-slot helper；不得向应用
宣称 compiler TLS 可用。
