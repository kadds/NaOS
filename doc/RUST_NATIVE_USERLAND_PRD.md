# PRD：零 mlibc 依赖的 Rust Native 用户态

- 状态：Draft
- 日期：2026-08-18
- 负责人：NaOS kernel / userland 维护者
- 关联：[总体架构](ARCHITECTURE.md)、[Capability 与 Invocation ADR](OBJECT_CALL_ADR.md)、
  [Rust Native TLS ABI ADR](RUST_NATIVE_TLS_ADR.md)
- 本文对应基线：当前工作树；本文只定义产品、ABI 和迁移契约，不实现代码。

## 1. 摘要

NaOS 需要支持以 Rust 编写的用户态原生程序和服务，并使它们不依赖 mlibc、POSIX fd
table 或 Linux syscall ABI。Rust 程序应直接在 NaOS 的 capability、channel、Invocation 和
NaoIDL protocol 边界上工作；mlibc 继续仅服务于既有 C/C++、BusyBox 和 POSIX 兼容程序。

第一版交付静态链接的 `#![no_std]` Rust 程序：它能作为 rootfs 中的普通 ELF 被启动，完成
native bootstrap，调用受支持的 native syscall，写出诊断信息并安全退出。第二版增加 Rust
`alloc`、RAII capability 封装和 NaoIDL Rust bindings。第三版正式支持静态 Rust compiler TLS
和 native threads；它以 `#[thread_local]` 为首个可用接口，仍不等价于移植 Rust `std`。

## 2. 现状与问题

### 2.1 当前实现事实

- 安装的 Rust 工具链没有内置 NaOS target。用户态 C/C++ 通过 static mlibc、`crt1.o` 和
  自定义 linker script 构建；现有 startup 固定调用 C ABI 的 `main()`。
- 内核将标准 ELF 初始栈和 auxv 交给进程。mlibc 在其 startup 中解析它，并在进入 C/C++ 用户
  代码前消费 native bootstrap。
- 目前 ELF loader 仅装载 `PT_LOAD`；`PT_INTERP` 没有动态链接实现。它把 program header
  信息经 auxv 交给进程，因此 runtime 可发现并复制 `PT_TLS` template，但 kernel 不应解析
  Rust TLS 内容。用户态切换时，内核将线程的 `tcb` 写入 x86-64 FS base。
- `clone` 已把经校验的 TCB 传入 child，且在 child 第一条用户指令前设置 FS base；这提供了
  per-thread TLS 的必要内核基础。Rust target 已设为 `has-thread-local=true` 与
  `singlethread=false`；runtime 的固定-slot `ThreadLocal<T>` 仍只是过渡 API，不能代替真实
  compiler TLS 的 `PT_TLS` smoke。
- native ABI 已提供 handle/channel、Invocation、bootstrap、memory map、进程创建、futex、
  thread creation 和 TCB 设置等基础机制。POSIX 文件、路径、fd 和 errno 语义属于 mlibc
  compatibility layer，而不是 native ABI。
- NaoIDL 现在同时生成 C++ public binding/UAPI 和模板驱动的 Rust canonical codec、protocol
  descriptor、typed client/server endpoint 及 resource ownership API。

### 2.2 要解决的问题

1. 使 Rust 编译器能为 NaOS 产生 ABI、code model 和 ELF layout 均正确的静态 executable，
   而不误用 `x86_64-unknown-linux-gnu` 的标准库或 Linux syscall 约定。
2. 提供一个不经过 mlibc 的 Rust startup、bootstrap、panic、内存分配和 native ABI binding。
3. 将 NaoIDL 的 canonical wire contract 同时提供给 C++ 和 Rust，避免 Rust 用手写 ordinal 或
   payload 绕过协议边界。
4. 为 Rust TLS 与多线程定义唯一的 x86-64 TLS/TCB ABI；它必须与内核 FS-base context switch
   和静态 ELF TLS relocation 一致。

## 3. 目标、非目标与成功标准

### 3.1 目标

- 新增稳定的 `x86_64-unknown-naos` custom Rust target；其产物是 x86-64 static ELF，使用
  NaOS linker script，且没有 ELF interpreter 或 runtime dynamic dependency。
- 新增最小 Rust native SDK：`naos-sys`、`naos-runtime` 和 `naos-idl`。
- `naos-runtime` 在进入应用逻辑前完成 native bootstrap；应用不需要、也不得调用 mlibc
  bootstrap 私有接口。
- MVP 支持 `core`；第二阶段支持 `alloc`，包括释放、realloc、对齐分配和分配失败的受控处理。
- Phase 3 支持 static `PT_TLS` 和 nightly `#[thread_local]`，并以同一 immutable template 为
  main thread 与 native child thread 创建隔离 TLS block。
- 所有 native handle 在 Rust 中有明确 ownership：关闭、duplicate、restrict、MOVE transfer
  和 invocation/responder 一次性消费规则由类型 API 表达。
- Rust binaries 进入既有 CMake → rootfs image → QEMU 验证流程，但链接时不输入 `libc.a`、
  `crt1.o`、libstdc++ 或任意 mlibc header。

### 3.2 非目标

- MVP/Phase 3 不移植 Rust `std`，不承诺 `std::fs`、`std::process`、`std::net`、`std::thread`、
  `std::thread_local!`、`libc` crate 或任意 Linux-only crate 可用。`std::thread_local!` 依赖
  `std` runtime；Phase 3 的可用语法是 nightly `#[thread_local]`。
- MVP 不提供 POSIX fd、路径解析、errno、`fork()` 或 shell 兼容层；需要这些语义的既有程序
  继续使用 mlibc。
- MVP 不支持 dynamic ELF、`dylib`、`cdylib`、plugin loading、`panic=unwind` 或 C++ exception
  interop。
- 本项目不替换 BusyBox、init 或现有 C/C++ 服务，也不以支持 Rust 为理由修改 native
  capability/Invocation 的基本语义。
- Phase 4 交付 capability-native Rust `std` 的最小实现切片：锁定 Rust fork/stage1 的 source tree、
  `build-std` 编译链、argv/env snapshot、time、native thread/parking/TLS destructor 和 `id`/`exit`。
  RandomSource、ProcessControl/SignalReceiver、File/Directory/Stream capability 尚未落地；random
  缺失时 abort，filesystem/network/Command/stdio 等 surface 明确保持 `Unsupported`，不得用 host Linux
  fallback 模拟它们。

### 3.3 可验收的结果

在干净构建和 `python3 util/run.py q --iso -n` 启动中：

1. `rust-smoke-suite` 被安装到 rootfs，并可由 opt-in 的 `/etc/init.sh` 手工触发；它完成
   `na_bootstrap`、alloc、NaoIDL、TLS/thread smoke，输出 `rust-smoke-suite: native bootstrap ready`
   诊断并正常退出。默认 rootfs
   不提供 `/etc/init.sh`，正常 init 不启动任何 test 或 smoke runner。
2. `readelf` 显示该 binary 为 static executable，未含 `PT_INTERP` 或 `NEEDED` dynamic
   dependency；链接 map/命令可证明没有 mlibc archive 或 crt object 输入。
3. `rust-smoke-suite` 能执行分配、realloc、对齐分配、释放和 allocation failure 路径；它不
   调用 legacy `brk`/`sbrk` 作为 allocator backing。
4. Rust NaoIDL client/server 的 canonical request/response、resource disposition、malformed
   message、peer-close 和 cancellation 行为与同一 schema 的 C++ binding 互操作。
5. TLS 阶段完成后，主线程和至少一个子线程能分别读写真实 `#[thread_local]` static；二进制含
   `PT_TLS`，FS base、template copy、thread exit 和 join 的负向路径均无 cross-thread 泄露、
   UAF 或 kernel fault。

## 4. 产品边界与总体架构

```text
Cargo workspace (root Cargo.toml/Cargo.lock)  CMake/rootfs packaging
  ├── naos-sys       raw native ABI  ───────────────────────────────┐
  ├── naos-runtime   startup/heap/TLS                                │
  ├── naos-idl       generated typed protocol bindings               ▼
  └── Rust smoke/services                                /bin/*rust*
             │                                                    │
             ▼                                                    ▼
      static x86-64 NaOS ELF ───────────────► kernel ELF loader + bootstrap
             │                                         │
             └──── native handles / channels / NaoIDL ─┘

mlibc / POSIX applications remain a separate compatibility path.
```

### 4.1 SDK crate 责任

| crate | 责任 | 禁止承担的责任 |
| --- | --- | --- |
| `naos-sys` | `#[repr(C)]` ABI structs、constants、minimal `unsafe extern "C"` syscall declarations、status types | POSIX wrappers、allocator policy、业务协议语义 |
| `naos-runtime` | `_start`、auxv/argv parsing、bootstrap、panic abort、allocator、owned handle、static TLS 与 native thread runtime | 路径/fd emulation、手写 NaoIDL payload |
| `naos-idl` | 从 `.naidl` 生成 UUID、ordinals、canonical codec、typed client/server endpoint 及 resource transfer API | 暴露 C++ generated headers、依赖 kernel-private headers |
| 应用/服务 | 业务逻辑、协议实现和明确的 capability 权限请求 | 裸 syscall number、伪造 handle、直接访问内核对象 |

所有 `unsafe` FFI 只允许位于 `naos-sys`、runtime 的少量 architecture module 或生成代码中；安全
crate 对调用方返回 `Result<_, Status>`，而不把 native transport error 折叠为 errno。

### 4.2 Rust target 与链接契约

目标文件 `targets/x86_64-unknown-naos.json` 至少冻结以下属性：

- `target_arch=x86_64`、little-endian、ELF、64-bit pointer，NaOS OS 名为 `naos`；MVP 不把它
  冒充为 Linux，也不因便利而宣称完整 `target_family=unix`。
- 使用当前 x86-64 code model 和 NaOS static linker script；linker driver 可复用
  `x86_64-pc-linux-gnu-g++`，但它只作为交叉链接驱动，绝不链接 Linux libc 或 Linux Rust std。
- 强制 `-nostdlib`/static/无 interpreter 的等价 link policy，产物必须与当前 kernel ELF
  loader 的 ET_EXEC 和 program-header 限制兼容。
- target 已设为 `has-thread-local=true`、`singlethread=false`；它必须持续受真实 `PT_TLS` smoke
  约束。仅有 JSON 开关不构成 TLS support，任何 smoke 失败都是发布阻断项。
- 通过 Rust fork 的本地 stage1 toolchain 与 `-Z build-std=core,compiler_builtins` 构建最小运行时；
  不使用 host target 的预编译 `core`/`std`。

根目录 `Cargo.toml`/`Cargo.lock` 是 workspace 的唯一项目配置与依赖锁定来源；各 Rust crate
仍位于 `naos/src/usr/rust`，便于和 userland 源码同树维护。`naos-idl/build.rs` 调用 IDL
compiler，并只把生成的 `.rs` 与 `bindings.rs` 写入 Cargo 的 `OUT_DIR`；源码树不保存生成
Rust 文件。Cargo 是 Rust 依赖解析与编译的唯一来源；CMake 只以 custom target 调用 Cargo、
声明输入/输出依赖，并将选定 executable 安装进 `${ROOT_FS_DIR}/bin`。全仓构建仍会为 C/C++
程序构建 mlibc，但 Rust target 的 link line 必须独立可审计。

## 5. MVP 设计

### 5.1 启动与 bootstrap

1. kernel 按现有 ELF x86-64 ABI 进入 `_start`，`rsp` 指向 argc/argv/envp/auxv 初始栈。
2. `naos-runtime` 的汇编入口保存栈指针，进入明确不访问 TLS、heap 或 mlibc symbol 的 early
   path。它从 auxv 找到 `PT_TLS`，以 raw native memory-map syscall 建立 main-thread TLS/TCB，
   并以 `set_tcb` 安装 FS base。
3. TLS ready 后，runtime 解析所需的 argv、envp 与 auxv，调用 public native `na_bootstrap`，验证版本、
   capability metadata、重复/缺失资源和 message size，再将资源封装为 owned Rust values。
4. runtime 调用应用的稳定 Rust entry，并在返回、panic 或 bootstrap failure 时以 native exit
   syscall 终止。初版 panic handler 输出有限诊断后 abort；不进行 stack unwinding。

普通 Rust 进程和 C/C++ 进程使用相同的 bootstrap channel contract。Rust runtime 不得从
`process_t`、kernel VFS 或 mlibc 私有状态推断 root、cwd、stdio 或 ServiceDirectory。

### 5.2 内存与 panic

- `core` MVP 可无 heap；需要动态内存的 crate 必须显式依赖 `alloc` 支持阶段。
- `alloc` 的 `GlobalAlloc` 用 native anonymous `memory_map`/`memory_unmap` 建立 backing，并实现
  `alloc`、`alloc_zeroed`、`dealloc`、`realloc`；所有 length、alignment 和 address arithmetic
  做 overflow 检查。不得把 `brk`/`sbrk` 作为新 native runtime 的长期接口。
- 初版的 allocation failure 和 `panic!` 都终止当前进程；保留可观测的有限诊断，不依赖
  formatter heap allocation。
- 编译 profile 固定 `panic=abort`、关闭要求外部 unwind runtime 的选项。支持 unwind 前必须先有
  单独的 unwinder、personality、DWARF/eh-frame 与跨 FFI 销毁语义设计。

### 5.3 ABI 与 NaoIDL binding

`naos-sys` 从受版本控制的 public ABI 输入生成或校验布局、size、alignment 和常量，不能维护一份
会静默漂移的手抄 C header 副本。其跨语言 ABI test 应编译并运行实际 C/C++ 与 Rust 小程序，验证
frame bytes，而不是以 Python 解析源代码替代行为测试。

`idl/naoidl.py` 的 Rust backend 必须复用 schema 的 UUID、method ordinal、canonical integer/byte/string
codec、request/response bounds 和 resource disposition 规则。Rust binding 与 module index 均由
Jinja 模板生成；生成的 Rust API 把 protocol descriptor、client/server endpoint、responder、invocation
和 oneway 的线性所有权编码进类型，禁止用户把已 MOVE 的 handle 再次用于另一 disposition。

当前实现覆盖仓库中的 examples/system schema：primitive、enum、定长 array/vector、struct、
bytes/string 的 inline/descriptor payload，以及 handle/client_end/server_end resource。NaoIDL v1
的 wire contract 没有逐元素 descriptor，因此 `vector` 的元素必须是定长类型；动态元素 vector
在公共 message 校验阶段明确拒绝，不允许生成出错误的 C++ 或 Rust binding。

### 5.4 Compiler TLS 与 native threads

Phase 3 采用 [Rust Native TLS ABI ADR](RUST_NATIVE_TLS_ADR.md) 的 System V AMD64 TLS Variant II
方案与 `naos_tls_abi_v1` 公共 TCB 前缀。`naos-runtime` 的 Rust-only implementation 从 auxv 的 `AT_PHDR`/`AT_PHENT`/
`AT_PHNUM` 定位唯一 `PT_TLS`，验证 `filesz <= memsz`、power-of-two alignment、template 位于
已装载的 user `PT_LOAD` 和发布时 TLS hard limit；随后复制 `.tdata`、清零 `.tbss`，把 TCB 放在
static TLS block 高地址端并将 FS base 指向 TCB。

runtime 用相同的 immutable descriptor 为每个 child 分配、复制并初始化 TLS/TCB，再把 TCB 传给
`clone`。kernel 只验证用户地址、在线程首次进入用户态前加载该 FS base，并在 context switch
恢复保存的值；它不解释 TLS bytes、DTV 或 Rust type。child trampoline 不得依赖“稍后再设 TLS”的
窗口。

mlibc CRT 和 thread path 必须使用同一个 `naos_tls_abi_v1` template/layout/TP，而不是另起
TCB；它可在公共前缀后维护 pthread、cleanup 与 C++ `thread_local` destructor extension。当前
mlibc provider 已通过 C++/Rust 同一 ELF 的联合 smoke 验证；纯 Rust binary 仍不链接 mlibc，但
ABI 保持兼容。dynamic TLS、`dlopen`、多 module TLS、Rust TLS destructor 与 Rust `std` 不在本
阶段；原固定-slot `ThreadLocal<T>` 也不得作为公开 compiler-TLS 承诺。`std::thread_local!` 与
`std::thread` 仍由 Phase 4 的 NaOS `std` port 决定。

## 6. 分阶段计划与退出条件

### Phase 0：工具链与链接探针

固定 `rust-toolchain.toml`、custom target、Cargo config 和 CMake integration；加入不依赖 mlibc
的 `no_std` link probe，检查 ELF header、entry、program headers 和 undefined symbols。

退出条件：开发机和 CI 能重建 `core`/`compiler_builtins`；产物被 rootfs image 纳入，且静态 link
审计通过。

### Phase 1：native `no_std` smoke

实现 `naos-sys` 和最小 `naos-runtime`：entry stack、bootstrap、status/error、有限日志、exit 和
至少一个 handle/channel 操作。添加 `rust-smoke-suite`，由 opt-in `/etc/init.sh` 启动并在 QEMU
serial log 留下可核查记录；正常 init 不触发测试。

退出条件：cold boot 下 Rust binary 成功 bootstrap 和退出；bootstrap 版本/资源错误、invalid handle
和 malformed frame 均产生受控 failure，没有 kernel panic。

### Phase 2：`alloc` 与 NaoIDL

实现 allocator 和 owned handle API；为 NaoIDL 增加 Rust generator，交付一个 Rust client 与一个
Rust server 或对 C++ server 的互操作测试。

退出条件：allocation/reallocation/free/overflow/failure 测试通过；Rust 与 C++ binding 可以就至少
一个系统 schema 双向调用，且 canonical bytes 与 resource MOVE/DUPLICATE 语义一致。

### Phase 3：compiler TLS 与原生线程

落实 TLS ADR：auxv `PT_TLS` discovery、raw early allocation、Variant II copy/zero、TCB install、
per-thread template clone 与无 TLS-after-free 的 exit tail；保持 target 的
`has-thread-local=true` 与 `singlethread=false`。提供最小 joinable thread API。

退出条件：真实 `#[thread_local]` binary 的 `PT_TLS`、main/child isolation、futex wake、
thread exit/join、stack/TLS cleanup 和 malformed/oversized TLS template 的负向路径通过
C++/Rust tests 与 QEMU 验证。

### Phase 4：Rust `std` NaOS PAL 最小实现切片

本阶段已经交付可构建、静态链接、`panic=abort` 的最小 NaOS `std` PAL。实现使用锁定的 Rust fork
submodule 和本地构建的 stage1 toolchain，保持 `target_os="naos"` 且不启用 Unix family；它不链接 libc、mlibc、host filesystem
或 host thread。完整 `std` 仍未宣称支持，所有未具备 native capability 的 surface 继续按本节的受控
Unsupported/abort 边界处理。

本实现的入口和验证点为 `naos/third_party/rust-naos/library/std`、`rust-smoke-suite`、`naos-runtime` 的 startup
snapshot/TLS/thread ABI，以及 CMake 的 `-Z build-std=std,panic_abort` target。下列契约中尚未有 native
ABI 的部分仍是后续设计承诺，不表示当前工作树已经提供该 API。

#### 6.4.1 Rust source、target 与错误边界

1. `target_os` 保持 `naos`，且不设置 `target_family="unix"`。实现直接位于锁定 Rust fork
   submodule 的 `library/std`，在 `std/src/sys` 的 target dispatch 中显式选择 `naos`，覆盖
   `pal`、`args`、`env`、`os_str`、`time`、`thread`、`sync/thread_parking`、`thread_local`、`random`、
   `process`、`stdio`、`fs`、`net`、`paths`、`pipe` 与 `fd`。不得通过把 NaOS 纳入 Unix cfg、链接
   `libc` crate 或调用 mlibc 来复用 Unix implementation。
2. Rust fork commit（当前为 `8bab26f4`，Rust `1.97.1` source）、bootstrap 版本（当前为 `1.96.0`）、stage1 toolchain、`library/std` 修改和
   `build-std` feature set 必须一起锁定。每次升级 Rust 都须重新盘点上述 dispatch 点及新增 PAL 依赖；无法为新依赖给出 NaOS 实现或受控 unsupported
   implementation 时，升级不得合入。

本地 toolchain 由 Rust fork 的 `x.py` 构建，不依赖 `std-overlay` 或 host `rust-src`：

```bash
RUSTUP_DIST_SERVER=https://static.rust-lang.org \
  rustup toolchain install 1.96.0 --profile minimal
bash naos/third_party/rust-naos/configure \
  --enable-local-rust \
  --local-rust-root="$HOME/.rustup/toolchains/1.96.0-x86_64-unknown-linux-gnu" \
  --enable-local-rebuild --disable-extended \
  --set build.build-dir="$PWD/build/rust-naos" \
  --set build.jobs=2 --set llvm.download-ci-llvm=true \
  --set rust.deny-warnings=false
python3 naos/third_party/rust-naos/x.py build --stage 1 library/std
python3 naos/third_party/rust-naos/x.py build --stage 1 src/tools/cargo
cmake -S . -B build -DNAOS_RUST_TOOLCHAIN_ROOT=\
  "$PWD/build/rust-naos/x86_64-unknown-linux-gnu/stage1" \
  -DNAOS_CARGO_EXECUTABLE=\
  "$PWD/build/rust-naos/x86_64-unknown-linux-gnu/stage1-tools-bin/cargo"
```

`configure` 生成的 `naos/third_party/rust-naos/bootstrap.toml` 是本机配置，不进入版本库；它的
`build.rustc`/`build.cargo` 必须指向 Rust `1.97.1` source 声明的 `1.96.0` bootstrap。CMake 通过 `RUSTC`
使用 stage1 compiler，stage1 Cargo 只负责驱动 `-Z build-std`。
3. NaOS PAL 只从 `naos-sys`、`naos-runtime` 和 generated `naos-idl` 取得 ABI；它不得导入
   `time.h`、`errno`、POSIX fd、`pthread`、`getenv`、`getrandom`、`fork` 或 host linker runtime。
   `std::os::unix` 因而不在 NaOS target 出现；NaOS 专有 API 置于拟议的 `std::os::naos`，并且只暴露
   已有 capability 权限可表达的操作。
4. PAL 把 native transport status 映射为恰当的 `std::io::ErrorKind`，同时以
   `std::os::naos::io::StatusError` 保留原始 `na_status_t`；它不得把 status 改写为 Linux errno，
   也不得丢掉 NaoIDL domain error。`OUTCOME_UNKNOWN`、peer close、cancel 和 object revocation
   必须作为不同的可观测失败保留，非幂等操作绝不自动重试。
5. `panic=abort` 继续是本阶段唯一 panic 策略。`std` 能编译不等于 `panic=unwind`、C++ exception
   互操作、backtrace/unwinder、dynamic TLS 或 dynamic ELF 获得支持；这些仍是单独的后续项目。

#### 6.4.2 启动参数与环境（`std::env`）

`naos-runtime` 在 TLS ready、但在任意 `std` 用户代码前，从 initial stack 捕获 argc/argv/envp。捕获
后的数据归 runtime 所有，不能保存指向 initial stack、mlibc environ 或 launcher 临时 buffer 的借用。

- 参数和环境值使用无 interior-NUL 的原始 OS bytes；`args_os`/`vars_os` 必须可保留非 UTF-8 值，
  `args`/`var` 按 Rust 的既定 UTF-8 failure 语义表现。环境 key 不得含 `NUL` 或 `=`，value 不得含
  `NUL`。无效 initial stack、未终止列表、长度溢出或超出发布时上限都使 startup 受控失败。
- runtime 将 envp 规范化为带锁、进程内共享的 environment map；launcher 传入的重复 key 以最后一项
  为准，`ProcessBuilder` 自己构造的环境绝不产生重复 key。`set_var`/`remove_var` 必须遵循锁定 Rust
  版本的安全签名与并发约束，且只修改这份 runtime map，不调用 C environ API，也不回写 initial stack。
- `std::env::{args,args_os,var,var_os,vars,vars_os,set_var,remove_var}` 以这份 map 为唯一来源。子进程
  只能取得 `ProcessBuilder` 明确选择的 snapshot；它不会因为父进程有 root、cwd、ServiceDirectory 或
  其他 capability 而隐式继承它们。
- `current_dir`、`set_current_dir`、`current_exe`、`temp_dir`、home directory 与 `PATH` 查找依赖
  文件/namespace PRD，Phase 4 必须返回 `Unsupported` 或保持未发布。纯 `OsString` 操作可用，但不能
  由此暗示路径可以被解析。

#### 6.4.3 时间与等待（`std::time`）

NaOS PAL 使用现有 native clock ABI 的两个明确时钟域：monotonic clock 驱动 `Instant`、parking、
futex、handle wait 和 `thread::sleep`；realtime clock 只驱动 `SystemTime::now`。runtime 要以 checked
seconds/nanoseconds conversion 包装 ABI。当前 clock 的 `tv_nsec` 只以微秒的整数倍出现；把 Rust
duration 交给 sleep/futex 时必须向上取整到该分辨率并在 wake 后重查 deadline，不能把微秒精度虚报为
纳秒精度或让 timeout 提前返回。

- `Instant` 不得由 realtime clock、wall-clock adjustment 或时区数据构造；超时先转换为 monotonic
  absolute deadline，再进入一次 wait。wake、spurious wake、cancel 或 peer-close 后必须重新计算剩余
  duration，禁止把墙钟跳变解释为 timeout。
- `SystemTime` 可以前后跳变；`duration_since` 的 backwards case 必须返回标准 `SystemTimeError`。
  NTP、RTC set、time zone、leap-second display 和日历格式化不是本阶段 API。
- `Duration` 到 ABI timespec/clock value 的转换必须检查负数、`tv_nsec` 范围、加法、乘法和 deadline
  overflow。无穷 deadline 以 native ABI 的 null/明确无 deadline 表示，不能用可溢出的最大时间戳
  代替。
- `thread::sleep`、`Condvar::wait_timeout`、`RwLock`/mutex parking 和 capability wait 必须共享上述
  monotonic 规则。它们允许正常的 spurious wake，却不能因为本节的 native signal event 而执行未知
  的异步 Rust handler 或伪造 Unix `EINTR`。

#### 6.4.4 线程、parking 与 Rust TLS destructor（`std::thread`）

Phase 3 的 `clone + PT_TLS + futex` 是本阶段的唯一底座，不能另建 pthread/host-thread 分支。
`std::thread::{spawn,Builder,current,yield_now,sleep,park,park_timeout,scope}` 与同步原语已经接入
NaOS futex PAL；后续仍按以下契约继续收紧边界：

1. 每个 spawned thread 在用户态分配独立 stack、join state、static TLS image 和
   `naos_tls_abi_v1` TCB；join state、返回值槽和 refcount 必须位于 child TLS mapping 外。child 在
   kernel 安装 TP 后才进入 Rust trampoline。`JoinHandle` 只能 join 一次；drop 将它 detach，绝不
   隐式 join。退出、spawn failure、join/drop race 和 process exit 都须恰好释放一次 stack、TLS 与
   join state。
2. `Builder::name` 是 runtime metadata，`ThreadId` 是不透明且不复用的 runtime identity，不能把可
   回收的 kernel TID 当作稳定 public identity。默认 stack 大小、最小值、上限和 guard page 必须由
   native VM contract 锁定；在可验证的 guard mapping 尚未存在前，非默认 `stack_size` 请求返回
   `Unsupported`，不得悄悄忽略。
3. `park` 是一个由原子 permit 加 futex 实现的 level-triggered 操作：先 `unpark` 后 `park` 必须立即
   返回，重复 unpark 合并为一个 permit，目标已经退出时 unpark 是无害 no-op。所有状态转换以
   acquire/release 排序并在检查与 sleep 间避免 lost wake。
4. compiler TLS 继续使用 ADR 的 static Variant II layout。为了支持 `std::thread_local!`，每个线程
   还须有 runtime-owned destructor registry：第一次初始化某 LocalKey 时注册一次；正常 thread exit
   以 reverse initialization order 执行析构，允许受限的再注册循环并有发布时上限。析构、`std` thread
   cleanup 和 runtime bookkeeping 全部完成后才可 unmap TLS，随后只走无 TLS reference 的 exit tail。
   panic abort 时进程终止，不承诺 `JoinHandle::join` 观察到 payload；dynamic TLS 仍不支持。
5. `available_parallelism` 只能查询 launcher 明确授予的 future `SchedulerInfo` capability 所报告的
   process allocation/quota；capability 缺失时返回 `Unsupported`，不能泄露或臆测全机 CPU 数。该
   capability 的 schema 不在本阶段落地。

#### 6.4.5 安全随机数（`std::sys::random`）

本阶段规定一个未来的 `RandomSource` typed capability，而不是 `/dev/urandom`、host syscall、RDRAND
fallback、当前 process 的 `AT_RANDOM` seed，或基于地址/时间的弱随机数。该 capability 由 launcher
在 bootstrap 中显式转移，或由已授予的 ServiceDirectory 以明确的 policy resolve；PAL 不得自行发现
全局服务。

- `RandomSource::fill(length) -> bytes` 的 canonical response 必须精确包含请求长度的 CSPRNG bytes，
  单次长度不超过 channel bound，并由 client 分块处理任意 `std` 请求。服务端不得接受 client raw
  pointer；请求、response、quota 与 cancellation 走 NaoIDL/Invocation 的正常 ownership 规则。
- 随机能力只表示读取 CSPRNG，不代表管理 entropy pool 或其他 system authority。service 在尚未达到
  cryptographic readiness、quota 拒绝、peer close、revocation 或 transport unknown 时必须明确失败；
  绝不可用可预测回退填充结果。随机 bytes 既用于 hashmap keys，也可供 `std` 的其他内部需求使用。
- Rust `std` 的内部 `fill_bytes` 无可恢复错误返回时，PAL 仅能写出不泄露 entropy 的有限诊断并按
  `panic=abort` 终止；它不得用零、旧 buffer、重试后的未知 invocation 或弱 PRNG 继续运行。no_std
  程序不因缺少 `RandomSource` 失去启动能力，只有启用相应 std capability profile 的程序需要它。

#### 6.4.6 原生 signal event，而非 POSIX async handler

本设计明确区分 capability readiness signal（如 `NA_SIGNAL_READABLE`）与进程控制 signal。前者仍只
用于 `Handle::wait`；后者不复用 `_s_sigsend`/`_s_sigmask`，因为它们是 PID/group、process-global
mask 的 legacy compatibility ABI，不能成为 Rust PAL 的授权或线程安全基础。

后续 schema 应提供以下两个 capability 角色，名称可在 schema review 时调整但权限和生命周期不得
改变：

- 有 `NA_PROCESS_RIGHT_SIGNAL` 的 `ProcessControl`/受限 `Process` capability 才能向目标发出
  `Interrupt`、`Hangup` 或受策略许可的 user signal，或执行不可拦截的 `Terminate`。发送者身份由
  kernel/current caller 取得；API 不接受可伪造 PID/TID。
- 目标进程在 bootstrap 中显式接收 `SignalReceiver`。可捕获信号按 kind 的 pending level 交付：每种
  kind 可合并但不会在 receiver readable 时被遗忘，`take_pending` 原子取走集合。队列满不会丢弃
  未处理 kind，也不要求在任意线程栈上注入 callback。`Terminate` 不进入此 receiver，仍由 process
  lifecycle 规则执行。

因此 `std::os::naos::signal` 只能暴露显式 wait/take 的 event API；NaOS `std` 不提供 C ABI
`signal()` handler、signal frame、per-thread `sigmask` 或 Unix restart/EINTR 语义。终端 job-control
和 mlibc POSIX signal compatibility 保持自己的协议边界，不能借此绕过 Process capability rights。

#### 6.4.7 进程与 capability launch（`std::process`）

立即可设计的 Rust process API 与路径无关。`std::process::{id,exit,abort}` 可直接建立在 current
process/exit ABI 上；拟议 `std::os::naos::process::{Executable,Bootstrap,ProcessBuilder,Child}` 则接受
一个带 executable 权限的 opaque `Executable` capability，而不是文件路径或 fd。

1. `ProcessBuilder` 接收 argv、上一节的 env snapshot、显式 bootstrap resource plan 和可选受限
   ProcessControl right。它创建 bootstrap channel，发起 deferred spawn，发送并确认 canonical
   bootstrap resource transaction，最后调用 `Process.start`。child 在所有资源、environment 与
   `RandomSource`/`SignalReceiver` 等可选 capability 都安装完前不得执行。任何失败必须 abort
   未启动 child 或关闭其 endpoint，且恢复未 commit 的 parent resources。
2. `Child` 独占可 wait 的 Process capability；`wait`、`try_wait`、exit status 和 stable diagnostic id
   使用 Process protocol，不以 PID re-lookup。只有持有相应 right 的 `Child::kill` 才发出上述
   `Terminate`；drop 不等待也不终止 child。可复制的观察 handle 必须在 capability restrict 时移除
   wait/signal rights，从而避免两个 Rust owner 争抢 reaping。
3. `fork`、`pre_exec`、Unix `CommandExt`、process-group shell compatibility 与把 open capability
   反向转换为路径均不支持。完整 `std::process::Command`、`Stdio`、`current_exe` 和 PATH search 依赖
   File/Directory/Stream 的语义，必须等文件系统 PRD 与 stdio adapter 通过 review 后另行启用；当前
   ProcessBuilder 不是这些 API 的暗中替代品。
4. 现有 spawn frame 中的 diagnostic path 只是当前 loader 输入，不是长期授权边界。用户态文件系统
   PRD 将 executable 演进为 MemoryObject capability 时，`Executable` wrapper 保持抽象而不暴露
   `fs::vfs::file`、root/cwd 或 kernel-private type。

#### 6.4.8 文件系统与网络占位

为让 NaOS PAL source tree 可构建，`sys::fs`、`sys::net`、path-to-file bridge、DNS、socket、pipe 和标准
stdio 必须各有受控 unsupported PAL implementation：构造类型可以保留 upstream API 所需的最小内部
表示，但任何会取得或使用外部 resource 的操作返回 `io::ErrorKind::Unsupported`。它们不得访问
kernel VFS、mlibc fd、`/dev/*`、host filesystem、host DNS 或 host socket。

下列设计是明确占位，不能因存在 bootstrap Directory/Stream capability 而提前标记支持：

| 领域 | Phase 4 状态 | 解除占位所需的权威设计 |
| --- | --- | --- |
| `std::fs`、`File`、metadata、directory iteration、path cwd | Unsupported | [用户态文件系统 PRD](USERSPACE_FILESYSTEM_PRD.md) 完成 capability File/Directory、MObj exec 与 Rust `io::Error` mapping review。 |
| `std::net`、DNS、TCP/UDP/Unix socket | Unsupported | 独立 network capability、address/DNS policy、backpressure、poll/cancel 与 credential PRD。 |
| `std::process::Command`、`Stdio`、PATH search | Unsupported | 上述 filesystem 设计和 Stream → `Read`/`Write`/stdio lifecycle adapter。 |
| dynamic loader、unwind/backtrace、C++ exception interop | Out of scope | 独立 loader/unwinder ABI PRD。 |

#### 6.4.9 Phase 4 支持矩阵与验收设计

| Rust surface / 依赖 | 设计状态 | Native contract | 不允许的替代 |
| --- | --- | --- | --- |
| `std::env` args/env map | 已实现最小切片 | initial stack owned snapshot + synchronized runtime map | mlibc environ、host env、隐式 capability inheritance |
| `std::time::{Instant,SystemTime}` 与 timed parking | 已实现最小切片 | native monotonic/realtime clock、checked conversion、futex timeout | realtime timeout、time zone、虚假纳秒精度 |
| `std::thread`、parking、`thread_local!`、Mutex/RwLock/Condvar/Once | 已实现最小切片 | Phase 3 clone/TLS/futex、external join state、destructor registry | pthread、host threads、TLS unmap 后 cleanup |
| `std` random/hash keys | capability 未落地 | 缺少 RandomSource 时 abort；无弱随机 fallback | `/dev/urandom`、RDRAND/clock/address fallback |
| `std::os::naos::signal` | future capability | 需要 rights-gated ProcessControl + level-triggered SignalReceiver | PID signal、async handler、`_s_sigmask` |
| `id`/`exit` | 已实现；Child future | native current PID/exit ABI | PID re-lookup、fork、implicit stdio/root/cwd |
| capability `Child` | future capability | 需要 Process capability + deferred bootstrap/start transaction | path Command、隐式 capability inheritance |
| filesystem/network/standard `Command` | 受控占位 | `ErrorKind::Unsupported`，无 host fallback | fd/path/host filesystem/socket |

后续实现必须至少以真实 Rust/C++ tests 和 QEMU boot 验证下列可观察结果：非 UTF-8 argv/env 保留、环境
snapshot 不跨 child 泄露、realtime 回拨不提前结束 monotonic timeout、park/unpark 无 lost wake、两条线程
的 `thread_local!` destructor 正确执行且无 TLS UAF、RandomSource failure 不产生弱 key、无 signal right
的进程不能控制目标、bootstrap failure 时 child 从未运行，以及所有 filesystem/network call 都明确
返回 Unsupported。不得以 source scan、host-target test 或“能编译 std”代替这些行为验证。

退出条件：本节的支持矩阵、Rust fork revision、stage1/build-std 锁定和占位边界经 review 确认；已实现
surface 必须通过真实 Rust/C++ tests 与 headless QEMU。custom target 可以构建、或某个 API 在 host 上可用，
均不足以把未实现 capability surface 声称为 NaOS `std` 支持。

## 7. 安全、错误与资源规则

- 所有 status 保持 native ABI 的非负 transport status；domain error 由 NaoIDL error set 表达。Rust
  API 不得把它们伪装成 Linux errno 或自动重试 `OUTCOME_UNKNOWN` 的非幂等调用。
- `Handle` 的 `Drop` 只关闭仍 owned 的有效 capability；duplicate/restrict/MOVE 必须使原 value
  的可用状态在类型层可见。Drop 期间不得 panic。
- 所有 user-controlled buffer length、resource count、alignment、channel size、canonical offset
  和 allocation size 均须在 Rust 边界检查，并在 FFI 前转换为已验证的 ABI frame。
- bootstrap、IDL decoder 和 allocator 是高风险 `unsafe` 边界；它们需要 fuzz/property tests、
  resource-leak tests 和 peer-close/cancellation 测试。
- Cargo dependency graph、Rust fork revision、bootstrap/stage1 toolchain 必须锁定。新增 crate 应优先 `no_std`；
  build script、proc macro 或 C toolchain 依赖要有明确的 host/target 划分和供应链审查。
- Phase 4 的 initial stack parser、environment map、TLS destructor registry、RandomSource client 和
  SignalReceiver client 也属于高风险边界。它们必须有 length/overflow、concurrent mutation、drop race、
  peer close、revocation 与 denied-rights 测试；不得在 process-wide lock、TLS unmap tail 或 capability
  transaction 中执行可重入用户 callback。
- `RandomSource` 和 ProcessControl 只能使用实际转移或显式 resolve 得到的 typed capability。PID、TID、
  ServiceDirectory locator、environment value 和 bootstrap metadata 都不是授权凭据，不能用来补足权利。

## 8. 验证矩阵

| 层级 | 必测场景 |
| --- | --- |
| 编译/链接 | custom target、静态 ELF、无 interpreter、无 mlibc/crt input、entry/auxv、undefined symbols |
| runtime | bootstrap 成功/版本不符/缺 capability、argv/env、panic abort、exit status、日志边界 |
| allocator | alloc/zeroed/realloc/dealloc、alignment、overflow、OOM、map/unmap 回收 |
| ABI | C/C++ ↔ Rust struct/frame layout、`naos_tls_abi_v1` TCB prefix offset、status mapping、native handle ownership、invalid pointer/handle |
| NaoIDL | canonical codec、bounds、resource MOVE/DUPLICATE、oneway/invocation/responder、peer close/cancel |
| TLS/thread | Rust 与 mlibc/C++ initial TLS、two-thread isolation、公共 FS/TCB layout、C++ destructor、join/exit/futex、invalid template、resource cleanup |
| std source/PAL | `target_os=naos` dispatch 全覆盖、无 Unix cfg/libc/mlibc/host fallback、unsupported fs/net surface 明确返回 `ErrorKind::Unsupported` |
| env/time | non-UTF-8 argv/env、duplicate env 规范化、concurrent environment mutation、child snapshot、monotonic timeout、realtime 回拨、conversion overflow |
| std thread/TLS | `thread_local!` destructor 顺序和再注册上限、park/unpark race、join/drop/exit race、stack guard、缺少 SchedulerInfo 的 `Unsupported` |
| random/signal/process | RandomSource exact length/entropy failure/no weak fallback、signal rights/receiver coalescing、deferred bootstrap rollback、Child wait/kill capability rights |
| QEMU | rootfs install、cold boot、Rust smoke、failure diagnostics、serial log 无 kernel fault |

遵从仓库测试规则：用真实 Rust/C++ 行为测试、编译和 QEMU boot 验证；不添加以 Python 扫描或解析
C++/Rust 源码来证明实现完成的测试。

## 9. 风险与决策记录

| 风险 | 决策 / 缓解 |
| --- | --- |
| 将 NaOS target 伪装为 Linux | target OS 明确为 `naos`；禁止链接 Linux std/libc，缺失能力显式返回 unsupported。 |
| “纯 Rust”实际通过 mlibc | CI 审计 Rust link command 和 ELF dynamic section；`naos-runtime` 不导入 `crt1.o` 或 libc archive。 |
| custom target 与 toolchain 漂移 | 锁定 Rust fork/stage1，CI 重建 `core`，将 target JSON 和 linker policy 纳入评审。 |
| 在 TLS ready 前触发 FS=0/错误 FS 访问 | 只允许 assembly/early runtime 在该窗口运行；先初始化 Variant II block 和 FS base，才允许任何可能访问 `#[thread_local]` 的 Rust 代码。 |
| 将固定-slot `ThreadLocal<T>` 当作 compiler TLS | Phase 3 以 `PT_TLS` 与真实 `#[thread_local]` smoke 为门槛；slot API 仅是过渡实现，不进入稳定 SDK contract。 |
| 手写 IDL bytes 漂移或 capability 泄露 | NaoIDL Rust backend 为唯一 typed API；互操作和资源事务测试作为发布门槛。 |
| 为了 ecosystem 提前引入 std/dynamic loader | 将 full std、unwind 和 dynamic ELF 明确拆为后续项目，MVP 保持 static + abort。 |
| allocator 权限或地址算术错误 | 只用 bounded native map API，集中 unsafe，执行 overflow/OOM/leak 测试。 |
| Rust source update 偷走 Unix PAL | 以锁定 Rust fork 的 `library/std` 和 dispatch inventory review 维护 `target_os=naos` 分支；禁止把 target 加入 `target_family=unix`。 |
| 随机服务不可用时继续运行 | RandomSource failure 对 `std` 是 abort path，绝不退回 RDRAND、时间、地址、零或旧 bytes；no_std 不受该 capability 影响。 |
| PID signal 产生越权或 async handler 破坏 Rust safety | signal 只经 ProcessControl right 与 explicit SignalReceiver 传递；不提供 PID/TID targeting 或任意栈上 callback。 |
| 子进程隐式继承 authority | ProcessBuilder 强制显式 bootstrap resource plan 和 env snapshot；deferred child 在 transaction commit 前不执行。 |
| TLS destructor 在解除映射后运行 | destructor registry、thread cleanup 与 join-state release 都在 TLS unmap 前完成，exit tail 禁止 TLS reference。 |

## 10. 已完成的首个实现切片

`rust-smoke-suite` 已包含 `#[thread_local] static`，runtime 从 auxv 复制 `PT_TLS` template、
设置 main-thread FS base，并用同一 static 验证 child 隔离值；同时覆盖基础 native、alloc、NaoIDL
和 join/detach。`naos-smoke-suite` 进一步把 Rust compiler TLS probe 链入 mlibc/C++ ELF，验证
公共 TCB、C++ destructor、join/detach 回收和 Rust/C++ TLS isolation。统一的 `rust-smoke-suite` 同时验证 env/time/PID、thread parking、TLS destructor
和受控 filesystem/network/Command Unsupported；该切片仍不承诺 dynamic TLS、`dlopen`、RandomSource、
SignalReceiver 或 capability Child。
