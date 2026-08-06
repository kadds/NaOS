# NaOS Capability Handle 与异步 Invocation IPC PRD

- 状态：Draft
- 目标版本：待定
- 适用范围：NaOS x86-64 kernel、系统调用 ABI、NaOS userland SDK、mlibc、系统服务与后续远程 broker
- 替代文档：旧版“原生对象协议调用接口”草案

## 1. 概述

NaOS 将以 process-local capability handle、bounded channel 和 async-first invocation 作为新的资源访问与消息通信基础。
该模型替代当前以 `file_desc`、`read/write/ioctl` 和弱类型资源表为中心的 native ABI，并为后续将 VFS、TTY、Display、
网络栈和设备服务迁移到用户态提供稳定边界。

核心模型如下：

```text
opaque process-local handle
└── scoped capability view
    ├── KernelView<P>
    ├── ClientEnd<P>
    │   ├── same-machine ServerEnd<P>
    │   └── user-space broker ──> remote machine
    ├── RawChannelEnd
    ├── MemoryObject
    ├── Invocation<M>
    └── Responder<M>
```

所有 protocol invocation 都以异步提交为基础：

```text
invoke_submit(target, method, request, resources, operation_budget)
    │
    ├── immediate admission failure
    │       INVALID_HANDLE / ACCESS_DENIED / WOULD_BLOCK / ...
    │
    └── Invocation<M>
            │
            ├── wait(deadline)
            ├── cancel()
            └── take_result()
                    ├── protocol response
                    └── transport outcome
```

用户态 service receive request 时获得一个不可复制、至多使用一次的 `Responder<M>` capability。Responder 取代公开
transaction ID，确保 reply authority、生命周期和最多一次回复都由 capability 模型表达。

本 PRD 只统一 capability、message ownership、invocation lifecycle 和 canonical protocol wire。它不承诺本地与远程调用具有
相同延迟或故障模型，也不在 kernel 中实现网络、全局服务注册、透明重连或分布式 exactly-once。

## 2. 背景与现状

当前实现已经存在一个不完整的通用对象表，但其语义仍被 fd/VFS 模型主导：

1. `resource_table_t` 将 `file_desc` 映射到通用 `kobject`，却不保存 rights、protocol scope、generation 或 entry state，
   [resource.hpp](../naos/includes/kernel/resource.hpp)。
2. `resource_table_t` 同时保存 process root 和 cwd，使通用资源管理依赖内核 VFS。
3. `handle_t<T>` 是内核引用计数智能指针，却与未来用户可见 capability handle 同名，
   [handle.hpp](../naos/includes/kernel/handle.hpp)。
4. `file_desc` 固定为 `i32`，`0/1/2` 被内核定义为 console，并由 `dup2`、fork 和 process clone 保留数字身份，
   [types.hpp](../naos/includes/kernel/types.hpp)、[task.cc](../naos/src/kernel/task.cc)。
5. `open/read/write/ioctl` 等 syscall 先查通用对象表，再将对象强制解释为 VFS file，
   [file.cc](../naos/src/kernel/syscall/file.cc)。
6. 用户地址仍可深入 file/pseudo/driver 路径，范围检查不能替代 fault-safe usercopy 与 immutable snapshot。
7. 现有 message queue 使用全局 key 和独立 syscall，不支持 capability transfer、peer lifecycle、protocol scope 或原子 resource
   delivery，[msg_queue.hpp](../naos/includes/kernel/mm/msg_queue.hpp)。
8. fork/clone 默认复制资源表；这会使未来的 admin capability、channel endpoint 和 outstanding invocation 被隐式继承。
9. POSIX fd、native object reference、IPC endpoint 和远程引用尚未形成明确分层。

旧版 PRD 试图通过同步 `object_call(handle, call_frame)` 替代 `ioctl`，但公开 call ID、隐藏 result record、同步优先和每条
message 携带 protocol ID 的模型不适合新的 async-first capability 架构。本 PRD 不兼容该未实现草案。

## 3. 设计决策摘要

| 主题 | 决策 |
| --- | --- |
| Native resource identity | 所有 native resource 使用 opaque `na_handle_t`；handle 数值永远属于当前进程。 |
| Handle 表示 | UAPI 使用不透明 `u64`；不编码 locality、type、rights 或 protocol。`0` 为 invalid。 |
| Capability view | Handle 引用对象的 protocol-scoped view，而不是自动暴露对象实现的全部接口。 |
| 权限 | Possession is authority；meta rights 与 protocol-specific rights 分离，派生只能削减。 |
| POSIX | fd table、`0/1/2`、`dup2`、`FD_CLOEXEC` 和 `O_NONBLOCK` 只存在于 mlibc compatibility layer。 |
| Process bootstrap | Native process 使用 spawn + bootstrap channel；不隐式 clone capability table。 |
| IPC primitive | Channel 是 bounded、FIFO、message-oriented transport，bytes 与 resources 原子提交。 |
| Endpoint ownership | Protocol/channel endpoint 不可 duplicate，只能 MOVE；同一进程线程可共享同一 entry。 |
| Invocation | 所有 two-way protocol call async-first；`invoke_submit()` 返回唯一 `Invocation<M>` handle。 |
| Reply | Server receive 获得 one-shot `Responder<M>`；公开 ABI 不使用 transaction ID。 |
| Result delivery | Invocation 持有完整 result；buffer-too-small 或 usercopy fault 不消费结果。 |
| Cancellation | Timeout、close 和 cancel 都不等价于 rollback；只有可证明未 dispatch 时结果确定为未执行。 |
| Deadline | Wait deadline 与 operation budget 分离；跨机器 invocation 必须有有限 operation budget。 |
| Retry | 默认不自动 retry；NOT_DELIVERED还需policy允许，UNKNOWN仅限schema声明幂等且policy允许。 |
| Protocol identity | 每个 incompatible protocol major 使用稳定 UUID；UUID 只在 capability acquisition 时使用。 |
| Protocol addressing | Invocation 只携带 method ID；protocol/version/features 已绑定到 handle scope。 |
| Wire | 所有 transport 共享 canonical little-endian value wire；resource 永远带外传递。 |
| Binding | `invoke_submit()` 可调用显式 scoped 的 KernelView 或 ClientEnd；binding class 和 transport failure 可观察。 |
| Remote | Remote capability 由用户态 broker 代理；kernel 不理解 node、网络身份、TLS 或远端 object ID。 |
| Data plane | 控制消息有界 snapshot copy；大数据使用 MemoryObject、shared ring、stream 或 pager。 |
| Kernel 边界 | Kernel 长期只保留调度、VM、IPC、capability、interrupt/DMA 等机制；系统策略迁往用户态。 |
| ABI 发布 | Stable protocol 必须由 NaoIDL codegen、manifest 和 compatibility checker 生成并验证。 |
| Migration | 终态删除 native fd/ioctl；实施过程保留有删除期限的 legacy shim，并保持每阶段可启动。 |

## 4. 产品目标与非目标

### 4.1 目标

- 建立统一、类型安全、可削减权限的 process-local capability table。
- 让 channel、memory、process、thread、interrupt、protocol endpoint 和 invocation 都以 handle 管理。
- 用 async invocation + one-shot responder 表达 request/reply 生命周期。
- 保证 message bytes 与 transferred resources 要么整体提交，要么完全不提交。
- 保证用户指针只在 syscall/usercopy 边界访问；request 在 submit 时形成 immutable snapshot。
- 保证 result 在完整 take 前由 invocation 持有，不因 output fault 重复执行业务 method。
- 让同一 NaoIDL schema 支持 kernel view、同机 service 和用户态 remote broker。
- 将 POSIX fd 约束隔离到 mlibc，避免污染 native capability ABI。
- 让 namespace、cwd、service discovery 和设备策略可迁移到用户态。
- 为 fuzz、trace、ABI compatibility test 和生成 bindings 提供单一 protocol 事实源。
- 保持 freestanding kernel：不依赖异常、RTTI、host standard library 或 IDL runtime。

### 4.2 非目标

第一版不要求：

- 实现跨机器 broker、网络认证或 remote capability federation；
- 提供分布式 exactly-once、透明重连或同步远程内存映射；
- 实现层级化 ResourceDomain/Job 配额；但所有 kernel allocation 必须有固定硬上限；
- 实现 priority donation；但 invocation 必须保留同机因果关系以便后续扩展；
- 一次性迁移全部 VFS、TTY、Display、network 或 driver 到用户态；
- 让任意 handle 都支持通用 `read/write`；
- 兼容旧版未发布 `object_call` 草案的 syscall number、frame 或 status 数值；
- 允许 stable system protocol 绕过 NaoIDL；
- 在 kernel 中维护全局字符串 service registry；
- 让 raw local handle number 出现在 canonical payload 或网络 wire；
- 为调用方提供可递归撤销所有 capability 后代的通用机制。

## 5. 术语

| 术语 | 含义 |
| --- | --- |
| object | 具有独立生命周期的 kernel object、service endpoint 或其他 capability backing object。 |
| object reference | Kernel 内部强引用；实现类型应由现有 `handle_t<T>` 重命名而来，不对用户可见。 |
| handle | 当前进程 capability table 中某个 entry 的 opaque token。 |
| capability view | `object + binding class + protocol scope + revision/features + rights` 的组合。 |
| binding class | `KernelView`、`ClientEnd`、`ServerEnd`、`RawChannelEnd` 等调用路径类别。 |
| protocol scope | 一个 protocol major，或 schema 显式声明的 composite protocol 集合。 |
| protocol descriptor | NaoIDL生成并由immutable handle引用的scope、method shape、rights policy和bounds metadata。 |
| meta rights | 操作 capability 本身的权限，如 DUPLICATE、TRANSFER、WAIT、INSPECT。 |
| protocol rights | 由 protocol schema 定义、用于 method authorization 的权限。 |
| channel | 两端唯一持有、bounded FIFO、message-oriented 的 kernel transport object。 |
| endpoint | Channel 的一端，或绑定 protocol role 的连接 capability。 |
| invocation | 一次已接纳调用的 client-side waitable/result-owning capability。 |
| responder | 与一次 invocation 配对的 server-side one-shot reply capability。 |
| snapshot | Submit 时从用户地址复制出的 immutable message bytes。 |
| disposition | Request 中描述 resource MOVE/DUPLICATE 和 rights attenuation 的带外记录。 |
| resource slot | Receive/take 成功后激活的新 process-local handle 及可信 metadata。 |
| operation budget | 整次 invocation 剩余执行时间；跨机器逐跳递减。 |
| wait deadline | 单次 wait 最晚阻塞到何时；不改变 invocation 生命周期。 |
| call outcome | Transport 对 method 是否可能执行的结论，如 NOT_DELIVERED 或 OUTCOME_UNKNOWN。 |
| protocol result | Server 明确返回的 response 或 NaoIDL 声明的 domain error。 |
| broker | 用户态受信任服务，将本地 protocol endpoint 映射到远程 session/proxy。 |
| canonical wire | 与 CPU ABI 和 transport 无关的固定 endian、固定宽度 value encoding。 |

## 6. 总体架构

### 6.1 分层

```text
Application typed API
        │
        ▼
Generated NaoIDL client
        │  knows protocol scope, revision and binding class
        ▼
invoke_submit(target, method, snapshot, resources, operation_budget)
        │
        ├── KernelView<P> ──> generated kernel dispatcher ──> kernel mechanism/temporary adapter
        │
        └── ClientEnd<P>  ──> bounded channel queue
                                  │
                                  ▼
                          generated user server binding
                                  │
                                  ▼
                              service logic
```

各层职责必须隔离：

- Application 不拼装 method ordinal、padding、resource index 或 transport envelope。
- Generated client 构造 canonical payload，并声明 resource ownership 与预期 rights。
- Kernel syscall 层只处理 handle lookup、snapshot、admission、queue、usercopy 和 resource transaction。
- Kernel channel 层不理解用户 protocol 的业务字段。
- Generated dispatcher 验证 schema、rights metadata、scope、method 和 value。
- Service/driver 接收强类型 request，不接触用户地址、wire padding 或 process-local handle number。
- Broker 处理网络 framing、认证、lease 与 proxy mapping，不重新解释业务 payload。

### 6.2 长期 kernel 边界

永久 kernel mechanism 建议包括：

- process、thread、scheduler 和 address space；
- MemoryObject、VMAR、pager 的基础 VM mechanism；
- capability table、channel、event、waitset/port 和 timer；
- interrupt、MMIO/I/O-port、DMA/IOMMU capability；
- exception、fault 与必要 debug mechanism；
- fault-safe usercopy 和最小 trace hooks。

以下能力最终应由用户态 service 提供：

- Directory、File、VFS 和具体文件系统；
- TTY、PTY、console manager；
- display policy、compositor 与多数 display service；
- device manager 和可隔离的具体 driver；
- network stack、socket service；
- service discovery、namespace policy 和 remote broker。

迁移期间允许 kernel adapter 实现相同 NaoIDL protocol，但 adapter 不得成为永久业务 ABI 依赖。

### 6.3 Binding 可辨识性

统一 `invoke_submit()` 不代表隐藏 transport：

- Capability acquisition 明确产生 `KernelView<P>` 或 `ClientEnd<P>`。
- Binding class 存在于可信 table metadata；需要诊断时可通过 INSPECT 查询。
- Kernel 不根据路径或 object name 自动寻找 service。
- Kernel 不在 peer crash 后把旧 handle 重新绑定到新 service。
- Client error type始终保留 transport outcome。
- Remote proxy 仍然只是一个指向本地 broker 的 ClientEnd，不是网络 handle。

## 7. Capability Handle 模型

### 7.1 UAPI 表示

```cpp
using na_handle_t = uint64_t;
inline constexpr na_handle_t NA_HANDLE_INVALID = 0;
```

UAPI 只冻结“opaque u64”和 invalid value，不冻结内部拆分。第一版 kernel 可以使用 slot + generation，也可以使用其他能够防止
stale-handle ABA 的编码。

禁止：

- 从 handle bit 推断 object type、binding、rights、scope 或 locality；
- 将 handle number 当作跨进程、跨 boot 或跨机器 identity；
- 应用自行构造目标 handle；
- 在 canonical payload 内保存 `na_handle_t`。

### 7.2 Capability table entry

建议内部模型：

```cpp
struct capability_entry
{
    object_ref<object> object;
    uint32_t generation;
    entry_state state;              // reserved / active
    binding_class binding;
    protocol_scope scope;
    uint32_t selected_revision;
    feature_set features;
    meta_rights_t meta_rights;
    protocol_rights_t protocol_rights;
};
```

要求：

- Lookup 只返回 ACTIVE entry。
- RESERVED slot 对同进程其他线程不可见。
- Slot allocator 不得在 generation 未更新时复用。
- Generation即将回绕时必须永久retire该slot或切换到不会复发旧token的epoch/cookie；进程存活期间不得重新发布曾经有效的handle值。
- Table entry 不保存 `O_NONBLOCK`、`FD_CLOEXEC`、append 或其他 POSIX I/O policy。
- OBJECT_REVOKED 是 backing object 状态，不等价于 entry 不存在。
- Dispatcher 运行时不得持有 capability-table lock。

### 7.3 Protocol scope

一个 handle 默认绑定一个 protocol major。对象实现多个 protocol 时，应签发多个 scoped handles。

例如同一个 PTY backing object 可以签发：

```text
Stream capability
TtyControl capability
PtyAdmin capability
```

只有 schema 显式声明的 composite scope 才允许一个 handle 调用多个 protocol 的 method。Composite 使用统一、无冲突的 method
ordinal space。

允许的 scope 变化只有收窄：

- 单 protocol -> 更窄 revision/features；
- Composite -> 其中一个 component 或更小 composite；
- Protocol rights -> 子集；
- 不允许通过 derive 发现或扩大对象的其他 protocol。

### 7.4 Rights

Meta rights 第一版至少包括：

| Right | 语义 |
| --- | --- |
| `DUPLICATE` | 创建独立的同 scope capability；目标 rights 只能削减。 |
| `TRANSFER` | 将 capability 以 MOVE 或 DUPLICATE disposition 放入 message。 |
| `WAIT` | 等待该 capability 声明的 signals。 |
| `INSPECT` | 查询 scope、rights、signals 和有限诊断 metadata。 |

Protocol rights 由对应 NaoIDL library 定义，例如 TTY 的 `QUERY_ATTRIBUTES`、`SET_ATTRIBUTES`，或 MemoryObject 的
`READ`、`WRITE`、`MAP`。避免使用含义覆盖所有对象的万能 `ADMIN`。

Authorization 顺序：

1. 解析 handle 并取得稳定 object reference。
2. 验证 binding class 与 protocol scope。
3. 验证 meta operation 所需 rights。
4. 验证 method 所需 protocol rights。
5. Generated dispatcher 再验证业务 schema 与 object state。

Possession is authority。路径权限、UID 或 service authentication 只决定 capability acquisition；后续 invocation 不重新走路径
ACL。用户态 service 可以在 connection establishment 时认证 caller，然后将身份策略转换为 scoped rights。

### 7.5 Handle 操作

Native handle layer至少提供：

| 操作 | 语义 |
| --- | --- |
| `handle_close` | 删除当前 table entry；不取消已接纳 invocation。 |
| `handle_duplicate` | 要求 DUPLICATE；创建独立 entry，scope/rights 只能削减。 |
| `handle_restrict` | 消费原 handle 并返回更窄 scope/rights 的新 handle；不产生额外 authority。 |
| `handle_get_info` | 要求 INSPECT；返回可信 scope、rights、signals 和 object state。 |
| `handle_wait_many` | 对多个 waitable handles 做 level-triggered wait。 |

Channel endpoint、Invocation 和 Responder 默认没有 DUPLICATE right。它们可以按各自规则通过 MOVE 转移。
Duplicate/restrict失败时source保持不变；`handle_restrict`只有在新entry可原子发布时才消费旧entry。

### 7.6 Close 与并发 lookup

`invoke_submit(h)` 和 `handle_close(h)` 以 capability-table transaction 的线性化点决定结果：

- Submit 先完成 lookup/rights snapshot：invocation 持有稳定 object reference，后续 close 不影响本次调用。
- Close 先完成：submit 返回 INVALID_HANDLE。
- Close 不等待已接纳 method，不修改其 rights snapshot，也不隐式 cancel。
- Slot 被回收时 generation 必须变化，旧 handle 永远不能命中新 object。

Duplicate、restrict、MOVE、close、result activation 和 receive activation 必须共用可证明原子的 table transaction API。

### 7.7 普通 delegation 与撤销

普通 capability 成功转移后，原授权者不能递归撤销 receiver 及其后代：

- Close 只影响当前 entry。
- MOVE 只表示 authority ownership 转移。
- DUPLICATE 创建独立 authority。
- Kernel v1 不维护 capability derivation tree。

需要撤销的授权必须显式通过 lease/proxy/gate object。销毁 backing object、device hot-unplug 或 lease 到期可以使全部引用观察到
OBJECT_REVOKED，但这属于 object lifecycle，不是普通 delegation rollback。

### 7.8 Object revocation

必须区分：

| 状态 | 含义 |
| --- | --- |
| `INVALID_HANDLE` | 本地 handle 已关闭、generation 错误或不存在。 |
| `OBJECT_REVOKED` | Handle entry 存在，但 backing object 不再接受操作。 |
| `PEER_CLOSED` | Channel 对端永久关闭。 |
| domain state error | Object 存在且可调用，但当前业务状态拒绝 method。 |

Revoked object 必须唤醒 waiters。Service restart 不会让旧 endpoint 自动连接到新实例；client 必须重新从 service directory 获取
capability。需要透明恢复时，由显式 stable proxy service 实现。

## 8. Process Bootstrap、Namespace 与 POSIX 边界

### 8.1 Native spawn

Native process creation 采用 spawn + bootstrap：

1. 创建空 capability table 的 child process。
2. Parent/loader 创建 bootstrap channel pair。
3. Parent 以 MOVE/DUPLICATE 明确选择 child 所需 capabilities，并削减 rights。
4. Kernel 启动 child 时只安装 bootstrap endpoint。
5. Child runtime 从 bootstrap message 建立 namespace、stdio、loader 和 service clients。

Native ABI 不承诺复制父进程 table，也不承诺内存中任意 handle value 在 fork 后有效。

### 8.2 Bootstrap 内容

Bootstrap protocol 至少能够传递：

- process root Directory capability；
- cwd Directory capability；
- service directory ClientEnd；
- loader/runtime capabilities；
- stdin/stdout/stderr 所需 Stream/File clients；
- argv、environment 和启动 metadata；
- 明确授予的其他 application capabilities。

Kernel 不保存 process root、cwd 或全局 `/dev`。这些属于 user runtime 与 service namespace。

### 8.3 用户态 namespace

`open("/a/b")` 的 native实现路径：

```text
mlibc/VFS client
    → root or cwd Directory handle
    → Directory.Lookup/Open protocol
    → next Directory/File ClientEnd
```

Service manager 负责 namespace policy、mount、service restart 和 remote directory proxy。Kernel 只保证 channel、handle、memory、
process 和 pager mechanism。

### 8.4 mlibc fd table

POSIX compatibility layer维护：

```cpp
struct posix_fd_entry
{
    shared_io_binding_ref binding;    // owns typed native handle; shared by dup aliases
    posix_fd_flags fd_flags;          // e.g. FD_CLOEXEC
    shared_open_state_ref open_state; // e.g. O_NONBLOCK / O_APPEND
};
```

要求：

- `0/1/2` 只是 mlibc table indices。
- `dup/dup2` 操作 mlibc entries，并按 POSIX 规则共享 open-description state。
- `FD_CLOEXEC` 是每 fd flag；`O_NONBLOCK/O_APPEND` 属于共享 open state。
- `read/write/poll/mmap` 根据 binding 调用 typed protocol/data-plane API。
- Native kernel handle table不支持“分配指定数字”。
- POSIX fork compatibility path必须由 runtime 为 fd-backed bindings显式建立child-side endpoints和必要runtime capability。
- Exec 只重建明确保留的 fd entries；其他 native capability 默认不继承。

迁移期间 legacy kernel fd syscall可以存在，但不得作为新 native SDK 使用，也不得新增依赖。

### 8.5 POSIX dup/fork 与 unique endpoint 的兼容桥

POSIX descriptor sharing 不得破坏 endpoint unique ownership：

- 同一进程内，`dup/dup2` 让多个 fd entry 引用同一个 ref-counted userland binding 和同一个 native handle；只有最后一个 fd
  reference 释放时才调用 `handle_close`。
- 跨进程 fork 不得对 `ClientEnd` 调用 `handle_duplicate`。Fd-bindable protocol 必须提供协议级 `CloneBinding` 或等价操作，返回
  一个新的 unique `ClientEnd`，但让两个 server-side connection 共享同一个 POSIX open-description state。
- File offset、status flags、pipe/socket state 等共享语义存在于 service-side open-description object，而不是 endpoint table entry。
- Fork runtime 在 child resume 前为全部可继承 fd 建立新 binding；任一步失败都关闭已创建 bindings，并使 fork 整体失败。
- 不支持 fork clone contract 的 native capability 默认不继承，也不能仅因其 handle value 可见而进入 child。
- 多线程 fork 所需 stop-the-world、in-flight wrapper 协调和 snapshot 细节按第 26 节延后，但不能改变上述 ownership 语义。

因此 parent/child 可以共享 POSIX open description，却始终各自持有不同的 process-local endpoint；queue、waiter 和 close race 不会
因为跨进程别名同一 endpoint 而失去线性化点。

## 9. Channel 模型

### 9.1 基本语义

Channel 由两个 endpoint组成。每端有独立、bounded FIFO receive queue。

Channel 是 message/datagram transport：

- 不提供 short read/write；
- 一次 send 对应一条完整 message；
- message bytes 与 resources 原子提交；
- endpoint 提供 READABLE、WRITABLE 和 PEER_CLOSED signals；
- raw channel与 protocol-scoped endpoint共享 queue mechanism，但 capability scope不同。

### 9.2 唯一 ownership

Endpoint：

- 没有 DUPLICATE right；
- 可以通过 MOVE 原子转交另一进程；
- 同进程多个线程可共享同一 table entry；
- transfer 与并发 send/receive 必须有单一线性化点；
- 最后且唯一的 endpoint handle关闭时，对端观察 PEER_CLOSED。

Transferred endpoint可能嵌套在其他channel message中。Kernel必须把process/kernel table中的handle视为ownership roots，并回收没有
任何root可达的in-transit endpoint/message component；递归queue销毁必须使用有界worklist，不能用无界kernel stack递归。

Server扩展并发应创建 per-client endpoint，并由用户态 dispatcher/worker pool调度；不得通过跨进程 duplicate同一 endpoint实现。

### 9.3 Send commit point

Send/invoke admission成功只表示：

> 完整 message 与全部 MOVE resources 已原子进入 peer queue，channel取得所有权。

它不保证：

- Server thread 已 receive；
- Schema 已验证；
- Method 已开始或完成；
- Peer 不会立即崩溃。

Queue满时：

- 返回 WOULD_BLOCK；
- 不创建等待发送的 invocation；
- 不消费 MOVE resources；
- 不产生 server-visible state；
- caller等待 WRITABLE后重试，且必须容忍竞争导致再次 WOULD_BLOCK。

### 9.4 Receive

Receive只处理 queue头部一整条 message：

1. 原子claim当前AVAILABLE queue head；同一message同时最多一个receive/discard owner。
2. 检查 caller capacity。
3. 为全部 returned handles预留不可见 slots。
4. 在不持有queue/table lock时复制 payload和可信 resource metadata。
5. 复制 receive metadata。
6. 在单一 table transaction中 dequeue、激活 handles和 responder。
7. 返回成功。

Buffer/resource capacity不足时返回所需数量，message保留。Usercopy fault同样保留。不得 partial dequeue、partial payload或
partial capability activation。

Capacity/fault/process-exit路径必须回滚reserved slots并释放claim。并发receive/discard在head被claim时返回WOULD_BLOCK或等待状态
变化；READABLE只是一条可竞争提示。任何失败线程写出的未激活token都必须因generation变化而永久无效。

Receiver可以显式 discard queue头部；discard销毁 message及其 resources。拒绝处理又不 discard会阻塞该 endpoint，这是
endpoint owner自己的 backpressure。

### 9.5 Signals 与等待

Signals采用 level-triggered语义：

| Signal | 条件 |
| --- | --- |
| `READABLE` | Queue中存在至少一条可 receive/discard message。 |
| `WRITABLE` | Peer存在且当前至少具备最小 admission credit。 |
| `PEER_CLOSED` | Peer endpoint永久关闭。 |
| `COMPLETED` | Invocation已产生可 take终态。 |
| `CANCEL_REQUESTED` | Responder对应 caller发出 best-effort cancellation。 |
| `OBJECT_REVOKED` | Backing object已撤销。 |

`handle_wait_many` 只观察 signals，不消费 message/result。WRITABLE是提示而非 reservation。

Native handle没有 blocking/non-blocking mode。需要等待的 wrapper使用显式 deadline；mlibc自行实现 POSIX `O_NONBLOCK`。

### 9.6 Protocol endpoint roles

Raw channel可以双向，但 NaoIDL endpoint分角色：

- `ClientEnd<P>`：提交 P methods。
- `ServerEnd<P>`：receive并实现 P methods。
- `Responder<M>`：对一次 M invocation回复。
- Callback需要 client显式传入另一 protocol的 ClientEnd。
- Subscription返回独立 event-stream endpoint。
- 普通 request/reply connection上不得隐式反向 RPC。

### 9.7 Endpoint 创建与不可变绑定

Raw channel 与 protocol endpoint 必须通过不同的创建路径：

- `channel_create` 只产生两个 `RawChannelEnd`。
- `protocol_endpoint_create` 原子产生一对 `ClientEnd<P>`/`ServerEnd<P>`。
- 创建操作引用 immutable `ProtocolDescriptor` handle，其中至少包含 protocol UUID、method shape/rights policy、可选
  revision/features 和 resource bounds；kernel做结构/上限验证，并snapshot为不可变table metadata。
- Scope、role、selected revision/features 和 method-rights policy 在创建时固定；之后只能通过 `handle_restrict` 削减 rights 或
  features，不能重新绑定。
- `RawChannelEnd` 不能 cast、upgrade 或 relabel 为 protocol endpoint；既有 endpoint 也不能改绑到另一 UUID、role 或 peer。
- `KernelView<P>` 只能由对应 kernel object 的创建/获取路径签发，不能从 channel 或任意 object handle 重新标记得到。
- Service directory 的 connect 流程负责创建或取得 matching pair，并分别 MOVE client/server ends；它不得复用连接到其他实例的
  endpoint。

Protocol UUID 证明 wire contract identity，不证明 provider identity。调用方对 provider 的信任来自获得 endpoint 的 capability
路径，例如受信任的 ServiceDirectory；任何进程都可以实现公开 protocol，但不能借此连接到另一个 service 的 backing object。

Phase 2 可以为 experimental Test protocol 使用 kernel 内置 descriptor；Phase 3 起 stable endpoint 必须引用 NaoIDL 生成的
descriptor。Descriptor 注册、共享和内部 compact scope index 的具体 ABI 在冻结 stable protocol 前确定。

## 10. Message 与 Resource Transfer

### 10.1 Transport metadata

Protocol request transport metadata至少包含：

| 字段 | 含义 |
| --- | --- |
| `method_id` | 已绑定 protocol scope内的 method ordinal。 |
| `flags` | request/one-way/event等已定义 transport形态。 |
| `byte_count` | Canonical payload长度。 |
| `resource_count` | 带外 resource数量。 |
| responder metadata | Two-way request对应 one-shot responder；不属于业务 payload。 |
| reserved | 必须为零。 |

正常 invocation不携带 protocol UUID、revision、local handle number或公开 transaction ID。

Network broker可以在自己的 framing中使用 connection-local transaction ID，但该值不进入 NaoIDL、local channel ABI或
application payload。

### 10.2 Snapshot ownership

Submit时：

- 先验证所有用户范围、count、overflow和全局上限；
- 复制 request bytes形成 immutable snapshot；
- 复制 resource dispositions；
- 验证全部 source handles、scope、rights和operation；
- 只有 queue/admission commit成功才消费 MOVE source；
- Submit返回后不再访问原用户 request地址。

禁止异步调用借用用户 buffer或 raw handle。

### 10.3 Resource disposition

Resource参数只允许：

| Operation | 语义 |
| --- | --- |
| `MOVE` | Commit成功后删除 source entry，由 message取得唯一 authority。 |
| `DUPLICATE` | 要求 DUPLICATE + TRANSFER，创建 rights/scope不高于 source的新 authority。 |

NaoIDL schema固定每个 handle字段的 ownership mode，caller不得临时把 MOVE改成 DUPLICATE或反之。Schema同时声明预期 scope和
required/max rights。

Payload中的 handle字段是从零开始的 resource index。禁止：

- BORROW；
- 进程 handle number内联；
- 重复、越界或未引用的 resource index；
- rights提升；
- 将 non-exportable resource交给 remote broker；
- 对 unique capability执行 DUPLICATE。

同一source handle在一条disposition array中最多出现一次；需要多个view时caller必须预先derive。Operation target endpoint不能同时
作为MOVE source，任何target/source alias冲突都在snapshot阶段以INVALID_ARGUMENT失败且没有side effect。

### 10.4 Receive-side validation

Kernel channel只验证通用 transfer安全和可信 metadata。Generated binding在把 handles交给业务实现前验证：

- 实际 scope与 schema预期一致；
- meta/protocol rights满足要求且不超过 schema允许范围；
- ownership与 resource count正确；
- method payload中的每个 index恰好引用一个 slot。

Validation失败时 binding关闭收到的 capabilities并按第 15 节处理 protocol violation。

### 10.5 Export class

每种 capability scope必须声明 export class：

| Class | 语义 |
| --- | --- |
| `LOCAL_ONLY` | 不能跨机器，如 process、thread、IRQ、VMAR、Invocation、Responder。 |
| `PROXYABLE` | Broker可持有真实本地 capability，并向远端签发 lease-backed proxy。 |
| `SNAPSHOT_COPY` | 只能复制 value/blob，不保留共享 identity。 |
| `SERVICE_DEFINED` | 由具体 service protocol定义远程语义。 |

MemoryObject默认不能提供透明 remote MAP。远程数据必须使用 snapshot、page/blob protocol或显式网络数据面。

## 11. Async Invocation 与 Responder

### 11.1 提交流程

```text
Client
  │
  ├─ invoke_submit(ClientEnd<P>, method, request)
  │       ├─ snapshot + validate + admission
  │       ├─ create paired Invocation<M>/Responder<M>
  │       └─ enqueue request with responder
  │
  └─ receives Invocation<M>
```

KernelView调用使用相同 client-side invocation生命周期，但 request交给 kernel dispatcher，而不是 channel peer。

`invoke_submit` 永不等待 method完成。Kernel method可以在 syscall返回前完成 invocation，但 client仍通过 invocation统一取得结果。

### 11.2 Responder

Responder规则：

- 不可 duplicate；
- 可在同机以 MOVE交给 worker；
- 绑定 method response scope；
- 最多成功 reply一次；
- reply成功原子消费 responder；
- Generated framework 可以用 `responder_fail` 原子消费 responder并完成 framework/transport error；业务错误必须走正常 reply；
- close未使用 responder产生 `OUTCOME_UNKNOWN + RESPONDER_ABANDONED`；
- cancellation和 operation deadline状态可作为 responder signals/context观察；
- 不可导出到远程机器；broker内部使用自己的 correlation mapping。

One-way notification不使用 `invoke_submit`，而使用 `invoke_send_oneway`；它不创建 Invocation/Responder，只报告 admission 是否
commit。Stable system protocol中，one-way只允许显式标记为 best-effort 的 notification/event；状态修改默认必须使用
two-way invocation。

`responder_fail` 只允许 generated binding、kernel adapter或 broker表达 framework终态：在可证明业务handler未进入时使用
NOT_DELIVERED，否则必须使用OUTCOME_UNKNOWN。它不编码protocol domain error。Kernel验证outcome/reason组合；任意
reply/fail/close/deadline race只有一个动作能赢得Responder线性化点。Invocation已关闭或deadline已赢时，late reply/fail返回
PEER_CLOSED并消费Responder。

Reply/fail在commit前先snapshot并验证全部frame/resources。普通admission failure不消费Responder或MOVE sources，允许修正后重试；
reply成功才同时消费Responder和MOVE sources。若reply sink已关闭，PEER_CLOSED终态消费Responder，但MOVE sources仍留在server
table。

One-way admission成功后没有后续delivery outcome；peer随后关闭、queued operation budget到期或binding拒绝message都不会回报
sender。其MOVE
resources已经转移，generated client不得因没有确认而自动重发。

### 11.3 Invocation ownership

Invocation：

- 不可 duplicate；
- 可在同机通过 MOVE转移 owner；
- 同进程多个线程可等待同一 handle；
- `take_result` 至多成功一次；
- 成功 take后进入 CONSUMED；
- 默认 LOCAL_ONLY；
- close表示 caller放弃结果，不表示 method rollback；已持有的result resources随Invocation一起销毁；
- pending时close撤销reply sink并可发送cancellation signal，server仍可能继续执行。

需要广播完成状态时使用 event/subscription，不复制 invocation。

### 11.4 状态机

内部状态：

```text
SUBMITTING
    │ admission commit
    ▼
QUEUED ── peer close/discard/cancel/budget expiry before receive ──> READY(NOT_DELIVERED)
    │ receive
    ▼
DISPATCHED ───── responder reply ──────────────────────────> READY(REPLY)
    │
    ├── responder closes without reply ────────────────────> READY(OUTCOME_UNKNOWN)
    ├── object/service loses ability to determine outcome ─> READY(OUTCOME_UNKNOWN)
    └── explicit proof of no execution ─────────────────────> READY(NOT_DELIVERED)

READY(...)
    │ successful take_result
    ▼
CONSUMED
```

External API不必暴露 QUEUED与DISPATCHED的实时差异；它至少暴露 PENDING、READY和CONSUMED，以及 READY中的 reply/call outcome。

一旦 admission commit，MOVE resources 已属于 queued message。Cancel、discard、peer close 或 pre-dispatch budget expiry 可以销毁
这些 resources，但绝不能在 caller table 中重建原 handle。

### 11.5 Endpoint 与 outstanding invocation

Endpoint close只影响新调用和未分发 request：

- ServerEnd关闭：QUEUED calls完成为 `NOT_DELIVERED + PEER_CLOSED`。
- Server显式discard queued protocol request：完成为 `NOT_DELIVERED + REQUEST_DISCARDED`。
- 已 receive并产生 responder的 calls继续存在。
- Worker可在没有 ServerEnd的情况下使用 responder回复。
- ClientEnd关闭：禁止新 submit；已持有 invocation仍可完成。
- Invocation最终状态由 responder/result决定，而不是由原 endpoint handle是否仍打开决定。

### 11.6 Result take

Invocation READY后持有：

- canonical response bytes，或 call-error metadata；
- response resources的强引用；
- 实际 byte/resource count；
- method/protocol metadata用于 generated decoder和 trace。

`take_result`：

- 原子claim READY result；并发take同时最多一个owner进入usercopy；
- capacity不足时返回所需 counts，不消费结果；
- usercopy fault不消费结果；
- 先预留全部 receiver handle slots；
- 复制 bytes、resource metadata与 outcome；
- 最后原子激活 handles并标记 CONSUMED；
- 不允许 partial success；
- 重复 take返回 ALREADY_CONSUMED。

Capacity/fault路径释放result claim并回滚reserved slots，使下一次take可重试；并发loser返回WOULD_BLOCK。不得在usercopy期间持有
Invocation state lock或capability-table lock。

该模型不需要公开 call ID、RETRY_RESULT或 process-global hidden result record。

### 11.7 Cancellation

- Wait timeout不触发 cancel。
- Invocation close只放弃结果；可以发送 best-effort cancellation signal，但不保证实现端停止。
- 显式 `invocation_cancel` 在 QUEUED阶段可移除 request并产生确定的 `NOT_DELIVERED + CANCEL_REQUESTED`。
- Cancel与`channel_receive`原子竞争；receive先线性化时调用已进入DISPATCHED，不能再报告未执行。
- DISPATCHED后 cancellation只通知 responder/handler；必须等 server明确回复、responder终结或operation budget到期才产生结果。
- 没有明确回复时 outcome为 OUTCOME_UNKNOWN。
- Cancel不消费Invocation；caller仍通过wait/take取得终态，或显式close放弃结果。
- `@cancellable` method的 generated handler接收 cancellation token。
- 需要回滚的业务使用领域级 BEGIN/COMMIT/ABORT，不依赖 transport cancel。

### 11.8 Deadline

两种时间约束：

| 类型 | 作用 |
| --- | --- |
| wait deadline | 只限制当前 wait；超时返回 WAIT_TIMED_OUT，invocation继续存在。 |
| operation budget | 限制整次 operation生命周期，并向 server/cancellation context传播。 |

跨机器 invocation必须有有限 operation budget。Broker逐跳递减 remaining budget，不能把某台机器的绝对 monotonic timestamp直接
发送到其他 node。

Operation budget从admission commit开始按kernel monotonic clock计时。它与receive/reply原子竞争：

- dispatch前到期：移除queued request并产生 `NOT_DELIVERED + OPERATION_DEADLINE`；
- dispatch后到期：立即产生 `OUTCOME_UNKNOWN + OPERATION_DEADLINE`，同时通知/撤销responder的reply sink；
- reply先线性化：保留definitive reply，deadline不再改写结果；
- deadline先线性化：late reply返回PEER_CLOSED并消费responder，不能覆盖已发布outcome。

### 11.9 Retry 与 idempotency

- Generated client默认不自动retry。
- NOT_DELIVERED只是允许retry的必要条件；还必须由client policy认可reason，且request/resources可重建。
- PROTOCOL_VIOLATION、REQUEST_DISCARDED、ACCESS_DENIED、显式cancel或caller operation deadline不得自动retry。
- OUTCOME_UNKNOWN默认禁止自动retry；只有`@idempotent` method和显式client policy都允许时才可重试。
- 含 MOVE resources的 request禁止自动 replay。
- DUPLICATE resources只有在 capability/session仍可重新派生时才可 replay。
- 非幂等 durable operation需要业务 request携带 operation ID，并由 server将去重记录与业务变更原子持久化。
- Transport invocation identity、broker attempt ID和业务 operation ID必须分离。
- Kernel不承诺 exactly-once across service restart或network partition。

## 12. Native Syscall ABI

### 12.1 返回约定

新 native syscall禁止混用有效值与负错误码。x86-64建议：

```text
RAX = na_status
RDX = value0
R8  = value1 when explicitly defined
```

Failure时 value registers清零。固定少量top-level handles优先通过value registers返回；receive/result中的批量resource handles允许
写入用户数组，但必须使用reserve-copy-activate transaction，fault时回滚且不发布任何有效handle。

Protocol response不通过 syscall正整数返回，而由 Invocation取得。

### 12.2 第一组语义接口

以下名称表达语义；当前 v1 syscall number 由 `naos/abi.h` 统一定义，并按实现顺序连续分配为 1..44：

```text
handle_close(handle)
handle_duplicate(handle, restriction)
handle_restrict(handle, restriction)
handle_get_info(handle, buffer)
handle_wait_many(items, deadline)

channel_create(options) -> raw_endpoint0, raw_endpoint1
protocol_endpoint_create(protocol_descriptor_handle, negotiation, client_rights) -> client_end, server_end
channel_send(raw_endpoint, send_frame)
channel_receive(endpoint, receive_frame)  // RawChannelEnd 或 ServerEnd<P>
channel_discard(endpoint)

invoke_submit(target, submit_frame) -> invocation
invoke_send_oneway(target, submit_frame)
invocation_cancel(invocation)
invocation_take_result(invocation, result_frame)

responder_reply(responder, reply_frame)
responder_fail(responder, execution_outcome, reason)
```

Raw channel send不创建 Invocation/Responder；protocol two-way call必须使用 `invoke_submit`，one-way notification必须使用
`invoke_send_oneway`。调用形式与method annotation不匹配时返回INVALID_ARGUMENT，且不产生message。
`channel_receive` 接受 `RawChannelEnd` 或 `ServerEnd<P>`；后者还会原子发布对应的 one-shot Responder。

### 12.3 Submit frame 语义

Versioned submit frame至少表达：

- `struct_size` 与已知 flags；
- method ID；
- request address/size；
- resource disposition address/count；
- operation budget；
- reserved fields。

要求：

- Frame和全部数组先 snapshot；
- size/count乘法和地址加法不得 overflow；
- 空范围要求 address为零；
- request、frame和disposition ranges不得以导致 TOCTOU/覆盖输出的方式重叠；
- 单 message与 resource count有固定 hard maximum；
- 未知 flags/reserved非零一律拒绝；
- Admission失败不创建 invocation、不消费 MOVE。

当前 v1 不保留旧版112-byte frame，也不预留历史 syscall number。ABI tests 必须持续保证 syscall number 唯一且连续。

### 12.4 Receive/result frame 语义

Receive与take frame至少表达：

- payload address/capacity；
- resource slot address/capacity；
- actual/required byte与resource counts；
- method/message metadata或 invocation outcome；
- responder/result handle的发布位置；
- reserved fields。

Capacity不足或 usercopy fault时 message/result保留。所有 returned handles只能在全部输出成功后激活。

### 12.5 Usercopy

必须提供真正 fault-safe的 `copy_from_user` 与 `copy_to_user`：

- canonical range check只能提前拒绝，不等价于页面可访问；
- input copy fault发生时不得产生可见 side effect；
- request在 admission前形成 snapshot；
- kernel、dispatcher和driver不得保存用户地址；
- output来自清零并完全初始化的 kernel buffer；
- receive/take output fault不得丢失 message/result；
- usercopy期间不得持有禁止 fault/sleep的 lock；
- fuzz和fault injection必须覆盖跨页、只读、unmapped和并发 unmap。

## 13. Error 模型

### 13.1 Submission/syscall status

表示 invocation未建立或当前 meta operation失败：

- OK
- INVALID_HANDLE
- WRONG_BINDING
- WRONG_SCOPE
- ACCESS_DENIED
- INVALID_ARGUMENT
- INVALID_MESSAGE
- BUFFER_TOO_SMALL
- WOULD_BLOCK
- WAIT_TIMED_OUT
- RESOURCE_EXHAUSTED
- FAULT
- OBJECT_REVOKED
- PEER_CLOSED
- ALREADY_CONSUMED
- NOT_SUPPORTED

Draft阶段不冻结数值；Phase 0由公开 UAPI header与tests冻结。

### 13.2 Invocation outcome

Transport call error使用至少两个维度：

```text
execution_outcome:
    NOT_DELIVERED
    OUTCOME_UNKNOWN

reason:
    PEER_CLOSED
    OBJECT_REVOKED
    OPERATION_DEADLINE
    CANCEL_REQUESTED
    REQUEST_DISCARDED
    RESPONDER_ABANDONED
    BROKER_FAILURE
    PROTOCOL_VIOLATION
```

例如“operation deadline在dispatch后到期且server没有reply”是 `OUTCOME_UNKNOWN + OPERATION_DEADLINE`，而不是一个无法判断
execution状态的扁平 ETIMEDOUT。

### 13.3 Protocol result

Server明确回复的 domain error必须由 NaoIDL error set声明。成功或 domain error都属于 definitive reply，不属于 transport
failure。

Native generated wrapper返回概念上的：

```cpp
result<Response, ProtocolError, CallError>
```

mlibc最后把这些状态有损映射为 errno/POSIX返回值。Native ABI不得把 errno作为 protocol事实源。

## 14. 并发、顺序与调度

### 14.1 Transport 顺序

- 每 endpoint queue保持 FIFO。
- 并发 send按 kernel线性化顺序入队。
- Endpoint MOVE不重排已提交 message。
- Receive只消费 queue头。
- Responder reply按 invocation identity匹配，不依赖 reply顺序。

### 14.2 Method 执行

Generated server runtime默认按 endpoint串行执行 method。只有 `@concurrent` method允许并发和乱序完成。

- 纯查询可以标记 concurrent。
- 修改配置默认串行。
- 多 endpoints指向同一 backing object时，对象级一致性由 service/object layer实现。
- 需要跨 endpoint原子语义时定义单个 transaction method，或显式 object-level serialization group。
- Server不得持有全局锁进行不受控下游 RPC。

### 14.3 Priority

v1不实现 priority donation。要求：

- Invocation内部保留 caller/responder因果关系和 trace context。
- 系统关键 service使用明确 scheduler policy。
- Payload不得自报可信 scheduler priority。
- Operation budget限制跨 service等待。
- 后续 donation只作用于同机有界 invocation chain，必须有最大深度和明确撤销点。
- Remote只允许 broker控制的 QoS class，不传播本机 scheduler priority。

## 15. Protocol Violation 与连接处理

必须区分业务错误和 wire攻击：

| 情况 | 行为 |
| --- | --- |
| Envelope长度、reserved、resource index或 responder metadata非法 | 若存在有效Responder则以`NOT_DELIVERED + PROTOCOL_VIOLATION`终结；关闭endpoint并记录。 |
| Strict unknown method | 关闭 endpoint。 |
| Flexible unknown method | Reply METHOD_NOT_SUPPORTED，连接继续。 |
| Canonical结构合法但业务值非法 | 返回 declared domain error，连接继续。 |
| Target ClientEnd缺少method rights | Kernel在enqueue前返回ACCESS_DENIED，不产生server-visible message。 |
| Resource字段的scope/type/rights与schema不符 | Binding以`NOT_DELIVERED + PROTOCOL_VIOLATION`终结Responder，关闭收到的handles和endpoint。 |
| Malformed one-way | 因无法回复，关闭 endpoint。 |
| Remote framing/auth失败 | Broker关闭 session及全部相关 proxy。 |

Kernel raw channel不强制用户协议，但 system service generated binding必须执行上述策略。

## 16. NaoIDL 与 Canonical Wire

### 16.1 Protocol identity

每个 incompatible protocol major拥有一个稳定128-bit UUID：

- UUID显式写入 schema并提交；
- UUID不由名称或 schema hash临时计算；
- 名称可以重构，不改变 identity；
- incompatible major获得新 UUID；
- repository registry记录 UUID、名称、owner和 stable/experimental状态；
- capability acquisition后，kernel可映射为内部紧凑 scope index。

Compatible revision和features在 connect/acquisition时协商，normal invocation不发送 UUID或version。

### 16.2 Canonical value rules

所有 transport共享同一 canonical encoding：

- primitive只使用固定宽度整数；
- 第一版 little-endian；
- 不使用 `bool`、native enum、bitfield、`long`、`size_t`、裸指针或引用；
- enum/bits有显式底层类型和 unknown策略；
- padding/reserved显式生成，输入必须为零；
- struct alignment与offset由 NaoIDL compiler确定，不使用host C++ compiler的自然布局；
- extensible struct只允许尾部追加安全默认字段；
- bounded vector使用相对payload起点的offset + element count，string使用offset + byte length；
- string为 UTF-8且不含隐式 NUL；
- resource field为 `u32 resource_index`；
- decoder验证 overflow、范围、alignment、count、overlap、padding与所有 bounds；
- kernel/local transport不得用未验证的 `reinterpret_cast` 代替 decoder。

### 16.3 Schema 构造

NaoIDL第一阶段需要支持：

- library；
- protocol UUID、revision和features；
- explicit method/field ordinal；
- fixed struct、array、enum、bits；
- bounded vector/string；
- error set；
- typed handle/resource；
- protocol composition；
- ClientEnd/ServerEnd类型；
- explicit reserved IDs。

Handle类型必须表达：

- expected scope/type；
- meta/protocol rights；
- MOVE或DUPLICATE ownership；
- export class。

### 16.4 Method annotations

至少包括：

| Annotation | 含义 |
| --- | --- |
| `@id(N)` | Stable method/field ordinal。 |
| `@rights(...)` | 调用所需 protocol rights。 |
| `@max_bytes(N)` | Method更严格的 payload上限。 |
| `@idempotent` | 允许按 retry policy安全重放。 |
| `@concurrent` | 允许同 endpoint并发执行。 |
| `@cancellable` | Handler接收 cancellation context。 |
| `@oneway_best_effort` | 无 responder、允许无业务确认的通知。 |
| `@experimental` | 不进入 stable ABI manifest。 |
| `@extensible` | 允许尾部兼容扩展。 |
| `@flexible` | 允许 schema未知值/method的指定兼容行为。 |

默认 method是 two-way、non-idempotent、serialized、non-cancellable和 strict。

### 16.5 Generated artifacts

每个 stable protocol至少生成：

```text
protocol.naidl
├── protocol_uapi.h
├── protocol_types.hpp
├── protocol_client.hpp/.cc
├── protocol_server.hpp/.cc
├── protocol_kernel_dispatcher.hpp/.cc   optional
├── protocol_metadata.json
├── protocol_abi_manifest.json
├── protocol_abi_test.cc
└── protocol_negative_tests.cc
```

Generated kernel code必须 freestanding、bounded、无异常、无 RTTI、无 host runtime，并且不访问用户地址。

### 16.6 Compatibility checker

Stable manifest必须拒绝：

- Protocol UUID复用或更换既有 identity；
- Method/field/enum ID复用；
- 已发布字段重排、删除、类型/offset/alignment变化；
- Request/response或ownership mode变化；
- Required rights扩大或安全语义改变；
- idempotent/concurrent/cancellable/one-way语义改变；
- Strict/flexible行为不兼容变化；
- 缩小已发布 bound；
- 删除 stable method而未 reserve ID；
- 让旧 client输入获得不同 authority或side effect的变化。

允许：

- 新 UUID定义新 major；
- 新 ordinal增加 method；
- Extensible struct尾部追加安全默认字段；
- Flexible enum/bits增加值；
- Experimental schema在 stable前调整。

### 16.7 发布纪律

- Handwritten Test/Echo protocol只用于 experimental mechanism验证。
- NaoIDL compiler、deterministic codegen和compatibility checker完成前，不发布 stable protocol。
- Stable wire/layout/IDs不得人工在多处重复声明。
- Compiler只在 host运行，不进入 kernel trusted computing base。
- Same schema/compiler version必须产生 byte-identical output。
- Build必须检测 stale generated files。

## 17. 数据面边界

Native ABI不提供任意 `read(handle)` 或 `write(handle)`。操作属于明确 object/protocol：

| 数据 | 机制 |
| --- | --- |
| 小型控制、metadata和对象创建 | Invocation message |
| Raw IPC datagram | Channel send/receive |
| 字节流 | Stream capability |
| 大块共享数据 | MemoryObject |
| 高频 producer/consumer | Shared ring + signals |
| File-backed mmap | MemoryObject + Pager |
| Readiness | Handle signals + wait_many |
| Network remote data | Broker-managed local buffer/stream protocol |

POSIX `read(fd)` 由 mlibc fd binding分派。小文件 I/O可以先使用有界 File.Read/Write；高频路径再协商 Stream或
shared-buffer capability。

Framebuffer、network packet、audio frame、大文件页和 DMA buffer不得放入普通 control message。Remote MemoryObject不能
透明 map，必须提供 snapshot/page/blob语义。

## 18. Service Discovery 与 Remote Broker

### 18.1 Service directory

目标架构中 Kernel 不维护全局字符串 registry，Service manager通过 Directory
protocol提供。当前迁移阶段先提供一个 kernel-backed `ServiceDirectory`
KernelView primitive：它只保存显式 MOVE 进来的 capability，并通过
`register`/`resolve`/`unregister` 管理生命周期；namespace policy 和实例选择
仍由后续 userland service manager负责。目标架构为：

```text
process namespace
└── ServiceDirectory ClientEnd
    ├── filesystem
    ├── console
    ├── display
    ├── device
    └── network
```

Connect语义：

```text
connect(name, protocol_uuid, revision_range, features, requested_rights)
    -> ClientEnd<P>(selected_revision, selected_features, attenuated_rights)
```

Service manager同时把对应 ServerEnd交给 service instance。Name选择实例；UUID验证接口 identity；capability rights表达授权。

### 18.2 Remote broker

Remote transport完全属于用户态：

- Broker建立双向认证和加密 session。
- Kernel只看到应用与 broker间的本地 ClientEnd/ServerEnd。
- Broker持有真实 capabilities，因此属于 trusted system service。
- Network wire使用 session-local、不可猜测 remote capability ID。
- Remote capability附带 protocol scope、rights、generation和lease。
- Raw local handle number永不上网。
- Session断开使相关 proxy handles进入 PEER_CLOSED/OBJECT_REVOKED。
- Reconnect建立新 session和新 capability identity。
- 第一版不支持offline long-lived bearer capability。
- Broker内部可以使用 transaction ID，但必须映射到本地 Invocation/Responder，不向应用暴露。

跨机器普通 capability MOVE只表示将真实 capability交给本地 broker；远端得到 proxy，并不获得底层硬件/object的物理所有权。

## 19. Legacy fd/ioctl 与子系统迁移

### 19.1 原则

- 最终删除 native `open/read/write/dup2/fcntl/ioctl` fd ABI。
- 迁移期间 legacy syscall shim有明确删除期限，不属于 stable native ABI。
- 新 native代码禁止新增 fd/ioctl依赖。
- 每个阶段必须 build、boot并通过对应 serial/QEMU验证。
- 不同时重写所有 subsystem；以可验证 vertical slice推进。

### 19.2 TTY/PTY

TTY是第一个真实 protocol用户，但不是第一个 IPC机制测试：

- 先完成 capability/channel/invocation/responder Test protocol。
- NaoIDL生成链路稳定后定义 experimental TtyControl与PtyAdmin。
- 现有 kernel TTY实现可通过 KernelView adapter暴露同一 schema。
- mlibc termios/PTY API转为 typed clients。
- 数据流使用 Stream/data-plane，不把终端字节塞入 control message。
- 用户态 TTY service成熟后，client切换到 ClientEnd binding。
- 最后删除 TTY ioctl switch。

现有 [PTY_PRD.md](PTY_PRD.md) 中的 fd/ioctl/O_NONBLOCK表述属于 legacy compatibility目标；实现时必须映射到本 PRD 的
native handle、Stream、signals和 protocol model。

### 19.3 Display 与 Console

- Display mode、buffer acquisition和hotplug使用 typed protocol。
- Pixel data使用 MemoryObject。
- Console manager是独立 service，不把 active terminal policy塞进 framebuffer driver。
- Kernel adapter只作为迁移实现。
- Buffer acquisition返回 scoped MemoryObject capability，MAP rights由 schema明确。

### 19.4 File/VFS

- Directory/File metadata使用 service protocol。
- Root/cwd由 user runtime handles表示。
- File data使用 bounded message、Stream/shared buffer或Pager。
- Service crash使旧 endpoints永久 PEER_CLOSED；client重新resolve。
- File-backed mmap只在 Pager和page ownership语义完成后上线。
- Remote file不得通过 kernel object invoke透明转发。

### 19.5 Process 与 stdio

- Native spawn只安装 bootstrap endpoint。
- Process identity使用 process-local `Process` KernelView capability；PID只作为 POSIX compatibility lookup，不是 native object identity。
- `Process.wait`和`Process.get_info`使用typed object-call；Process capability在父进程reap后继续保持backing process state，直到最后一个capability关闭。
- `Process` KernelView同时是 job-control object：`get_job_control_info`、`get_process_group`、`get_session`、`set_process_group` 和 `set_session` 覆盖 POSIX 的 session/process-group 操作；查询受 `INSPECT` protocol right 保护，变更受独立的 `JOB_CONTROL` protocol right 保护。
- `waitpid(pid > 0)`由mlibc获取对应Process capability并调用`Process.wait`；`waitpid(-1)`和process-group过滤通过当前进程的`Process.wait_children` typed method完成，不再回退到旧child-wait syscall。
- `setsid`、`getpgid`、`setpgid` 和 `getsid` 不再保留为 native syscall；mlibc 仅使用 PID compatibility lookup 获取 `Process` capability，随后通过上述 typed methods 完成 POSIX 适配。
- mlibc从 bootstrap把stdio capabilities装入 fd 0/1/2。
- POSIX fork走专用 compatibility path。
- Exec按 mlibc fd flags重建继承集合。
- Invocation、admin capability和临时 memory默认不继承。

## 20. Hard Limits 与 Backpressure

v1暂不实现 ResourceDomain/Job，但以下资源必须有固定 hard limits：

- 每进程 active + reserved handle数；
- 每 endpoint message count；
- 每 endpoint queued bytes；
- 每 endpoint queued resources；
- 每进程 outstanding invocation数；
- 每进程 invocation request/result bytes；
- 单 message bytes；
- 单 message resources；
- 全局 IPC object和memory上限；
- 单 protocol通过 schema声明的更小上限。

初始建议保留64 KiB single-message hard maximum和不高于64个 resources；最终常量必须在 Phase 0/1测量后确定并进入公开 limits
query与tests。Queue defaults不是 stable ABI，可由 kernel配置和service policy收紧。

任何 allocation必须在业务/dispatch side effect前完成 admission。达到限制时返回 RESOURCE_EXHAUSTED或 WOULD_BLOCK，不允许
partial commit。

内部 accounting API不得硬编码为永远按 PID计费；v1可以由 process sponsor，后续可替换为层级 ResourceDomain而不改变公开
message/capability ABI。

## 21. 可观测性

Trace至少记录：

- caller process/thread；
- target binding class和protocol UUID/name；
- method name/ordinal；
- invocation内部 ID；
- queue/admission/dispatch/reply/take timestamps；
- request/response byte与resource counts；
- rights/scope rejection；
- operation budget与cancel事件；
- terminal execution outcome和reason；
- broker session/attempt correlation（由broker记录）；
- retry attempt和business operation ID（若 protocol提供）。

默认不记录 payload。Schema可以为安全字段生成redacted trace descriptor。

Kernel应提供：

- capability table debug dump，受INSPECT/debug权限保护；
- endpoint queue depth与high-water marks；
- outstanding invocation/responder计数；
- protocol violation与resource exhaustion counters；
- stale-handle/generation failure counters。

## 22. 测试要求

### 22.1 Capability table

- Invalid、stale generation和closed handle；
- Slot reuse不能产生ABA；
- 以缩小generation宽度的test configuration强制wrap，验证slot retire/epoch策略不会复活旧token；
- Duplicate/restrict只能削减scope/rights；
- Unique object拒绝duplicate；
- Raw channel不能relabel为protocol endpoint，endpoint binding创建后不可改变；
- Submit/close、MOVE/close、duplicate/close并发线性化；
- RESERVED slot不可被其他线程lookup；
- Activate/rollback在fault与process exit下不泄漏；
- Object revoked与invalid handle错误区分。

### 22.2 Channel

- FIFO与并发send线性顺序；
- Queue byte/message/resource边界；
- Queue满时WOULD_BLOCK且source handles不消费；
- Bytes/resources整体commit或rollback；
- Endpoint MOVE不重排message；
- Receive buffer少1字节、少1 resource slot和zero capacity；
- Usercopy fault保留message；
- 并发receive/discard与fault只能有一个queue-head claim owner；
- Explicit discard关闭resources；
- Target endpoint作为MOVE source和重复source disposition被原子拒绝；
- 多channel互相携带endpoint形成的无root in-transit component可回收，close cascade不耗尽kernel stack；
- Peer close前后send/receive race；
- READABLE/WRITABLE/PEER_CLOSED level-triggered signals；
- Slow client不能突破per-endpoint hard limits。

### 22.3 Invocation/Responder

- Submit成功始终返回唯一 invocation；
- Responder不能duplicate且只能reply/fail一次；
- Responder reply/fail/deadline race只有一个终态；late reply不能覆盖outcome；
- Reply admission fault保留Responder/MOVE sources；late PEER_CLOSED消费Responder但保留MOVE sources；
- Responder MOVE给worker后仍可reply；
- ServerEnd关闭时queued call -> NOT_DELIVERED；
- Dispatched responder不受endpoint close影响；
- Responder close without reply -> OUTCOME_UNKNOWN；
- Cancel before dispatch -> `NOT_DELIVERED + CANCEL_REQUESTED`；
- Cancel after dispatch不伪报未执行；
- Wait timeout不改变invocation；
- Operation budget在dispatch前后行为；
- Take buffer不足/fault后可重试且method不重执行；
- 并发take与fault只能有一个result claim owner；
- Successful take只激活一次response handles；
- Invocation MOVE后由新owner take；
- Process exit清理未持有结果。
- Invocation close释放已缓存response resources并撤销reply sink。

### 22.4 Protocol/Wire

- UUID/revision/feature negotiation；
- Method/field explicit ordinal；
- Canonical size/alignment/offset golden tests；
- Integer/offset/count overflow；
- Variable region越界和非法overlap；
- Reserved/padding非零；
- Resource index缺失、重复和越界；
- Wrong scope/type/rights；
- Strict/flexible unknown behavior；
- Structural violation关闭endpoint；
- Domain validation error保持connection；
- Same schema在不同transport产生相同canonical payload；
- Compiler deterministic output与stale generated file检测；
- ABI manifest拒绝所有不兼容diff。

### 22.5 Security/Fuzz

- Frame、payload、dispositions、result slots的null/unmapped/read-only/cross-page buffers；
- Concurrent unmap/usercopy fault injection；
- Random canonical payload不得panic、越界或泄漏kernel data；
- Malicious resource count和queue pressure；
- Rights提升、scope扩大和forged responder；
- Remote broker framing与session capability ID fuzz；
- Driver/service不接触user pointer；
- Output padding与失败路径全部清零；
- Repeated protocol violation不会造成unbounded log/memory use。

### 22.6 POSIX 与系统验证

- mlibc fd 0/1/2 bootstrap；
- dup/dup2与shared open state；
- dup不duplicate native endpoint；fork为fd创建fresh endpoint并共享service-side open description；
- FD_CLOEXEC与exec；
- POSIX fork compatibility binding重建；
- TTY/PTY、shell和BusyBox基本交互；
- poll/select映射到native signals；
- Debug与Release完整build；
- QEMU BIOS/UEFI至少一个目标boot；
- Serial log无capability leak、protocol violation或usercopy panic；
- Legacy syscall逐阶段删除后仓库扫描无新增裸ioctl/native fd依赖。

## 23. 实施阶段

### Phase 0：内部语义与 ABI 基础

- 将内核 `handle_t<T>` 重命名为 `object_ref<T>` 或等价名称。
- 建立opaque u64 handle UAPI与status/value多寄存器返回约定。
- 将 `resource_table_t` 重构为generation、ACTIVE/RESERVED、scope和rights aware capability table。
- 实现fault-safe usercopy、range/overflow helper和fault injection。
- 将root/cwd从capability table设计中剥离，但legacy路径可暂时适配。
- 保留旧fd syscall shim，禁止新native代码使用。
- 冻结第一组handle ABI layout和status数值前先完成layout tests。

Exit gate：

- Kernel build/boot；
- stale handle、并发close和reserved slot tests通过；
- 旧shell/TTY路径仍可运行；
- 无行为变化的legacy shim可回归。

### Phase 1：Raw Channel 与 Resource Transfer

- 实现unique channel pair、bounded FIFO和signals。
- 实现raw send/receive/discard。
- 实现queue-head claim、迭代式close cascade和无root in-transit endpoint graph回收。
- 实现MOVE/DUPLICATE disposition与rights attenuation。
- 实现receive reserve-copy-activate transaction。
- 加入per-process/per-endpoint/global hard limits。
- 暂不实现protocol invocation。

Exit gate：

- Channel完整负向测试与fuzz；
- Queue pressure不泄漏memory/handles；
- Peer close与transfer race通过；
- Nested endpoint transfer/cycle测试无永久object leak或kernel stack递归；
- QEMU boot无回归。

### Phase 2：Async Invocation/Responder 闭环

- 实现 `invoke_submit`、Invocation与one-shot Responder。
- 实现immutable ProtocolDescriptor binding和typed endpoint pair创建；RawChannelEnd不可升级。
- 实现ServerEnd receive、reply、cancel、operation budget和result take。
- 使用手写experimental Test/Echo protocol。
- 实现KernelView和ClientEnd两种binding class。
- 实现分层call outcome。

Exit gate：

- Test protocol覆盖queued/dispatched/replied/unknown/consumed全状态机；
- Output fault不重复执行method；
- Responder reply-at-most-once；
- Endpoint close不错误取消dispatched call；
- p50/p99、allocation count和10,000次循环无泄漏报告。

### Phase 3：NaoIDL Bootstrap 与 Safety

- Parser、typed semantic IR、UUID/revision、explicit IDs和fixed types。
- 生成Test protocol client/server并替换手写binding。
- 增加resource ownership、rights、bounded vector/string与canonical decoder。
- 生成ABI manifest、compatibility checker、metadata和negative tests。
- 保持全部protocol experimental。

Exit gate：

- Generated Test protocol通过Phase 2同一测试集；
- Generated ProtocolDescriptor替换Phase 2内置experimental descriptor；
- Deterministic output与manifest diff测试；
- Kernel不链接compiler/runtime；
- Fuzz generated decoder无panic/leak。

### Phase 4：Experimental TTY Kernel Adapter

- 定义experimental TtyControl、PtyAdmin和Stream相关schema。
- 现有kernel TTY通过KernelView adapter实现typed methods。
- mlibc termios/PTY wrapper迁移，legacy ioctl保留fallback。
- Control message与stream data plane分离。
- 验证signals/poll映射。

Exit gate：

- Shell、TTY、PTY与BusyBox基本场景通过；
- Native wrapper不使用裸method ID；
- TTY output fault/cancel/hangup tests通过；
- 尚不发布stable TTY ABI。

### Phase 5：Bootstrap、Service Directory 与用户态 fd Table

- 实现 native spawn bootstrap channel：child 以空 capability table 创建，只接收 bootstrap endpoint；parent
  通过 MOVE executable/endpoint、DUPLICATE root/cwd/service/stdio capabilities 完成启动事务。
- 用户态service manager提供namespace和protocol connect。
- mlibc建立fd table、stdio bootstrap和typed I/O binding；无 file-actions/attributes 的 `posix_spawn` 已走 native
  spawn，兼容 fork/exec 路径保留给 POSIX 特性较完整的调用。
- POSIX fork/exec compatibility path显式管理handles。
- Root/cwd迁出kernel resource table。
- 以测试service验证ClientEnd binding。

Exit gate：

- Native child只靠bootstrap启动；
- fd 0/1/2、dup2、FD_CLOEXEC、O_NONBLOCK行为通过；
- Namespace capability隔离；
- Service crash/re-resolve语义通过。

### Phase 6：用户态 System Services 与数据面

- 迁移TTY/Console/Display中的可下放策略。
- 实现MemoryObject、shared ring和必要Pager机制。
- 以只读File/Directory service开始VFS迁移。
- Driver逐步使用IRQ/MMIO/DMA capabilities。
- 对kernel adapter与user service运行同一protocol测试集。

Exit gate：

- Kernel与user binding行为一致；
- 大数据不经过control message；
- Service restart不重绑旧endpoint；
- 性能达到各子系统预算。

### Phase 7：Legacy 删除

- mlibc、BusyBox和NaOS userland不再直接依赖旧native fd/ioctl syscall。
- 删除kernel `file_desc -> kobject`通用路径、ioctl_context和pseudo ioctl switch。
- 删除native open/read/write/dup2/fcntl/ioctl legacy ABI。
- 更新ARCHITECTURE、PTY和migration文档。
- 仓库扫描阻止新增legacy调用。

### Phase 8：Remote Broker（后续）

- 用户态authenticated session与proxy capability table。
- UUID/revision negotiation、lease、remote rights attenuation。
- Network framing、attempt correlation和finite operation budget。
- 只开放显式PROXYABLE/SNAPSHOT_COPY scopes。
- 注入partition、reconnect、late reply和broker crash测试。
- 不将remote broker作为本地IPC或fd迁移的前置条件。

## 24. 验收标准

| 编号 | 条件 |
| --- | --- |
| CAP-001 | `na_handle_t` 是process-local opaque u64，UAPI不编码type/locality/rights。 |
| CAP-002 | Capability table具有generation、ACTIVE/RESERVED、scope和两层rights。 |
| CAP-003 | Stale/wrapped handle不能命中新object；并发close不会破坏已接纳invocation。 |
| CAP-004 | 普通capability只能削减rights/scope，不提供隐式递归撤销。 |
| CAP-005 | Endpoint、Invocation和Responder不可duplicate。 |
| CAP-006 | Native spawn不隐式clone capability table。 |
| CAP-007 | Root、cwd和service namespace通过bootstrap handles建立。 |
| CAP-008 | POSIX fd数字身份与O_NONBLOCK只存在于mlibc。 |
| IPC-001 | Channel send对bytes/resources整体commit或完全失败。 |
| IPC-002 | Queue有硬上限，满时fail-fast且不消费MOVE resources。 |
| IPC-003 | Receive不足/fault保留完整message；explicit discard才丢弃。 |
| IPC-004 | Endpoint MOVE与并发send/receive有明确线性化语义。 |
| IPC-005 | Raw channel不可重新标记为typed endpoint；endpoint scope/role/revision绑定不可变。 |
| IPC-006 | Receive/result claim防止并发重复发布；无root in-transit endpoint graph可回收。 |
| INV-001 | Protocol call async-first，submit成功返回唯一Invocation。 |
| INV-002 | Server reply只通过one-shot Responder，公开ABI无transaction ID。 |
| INV-003 | Wait timeout、operation deadline、cancel与close语义彼此分离。 |
| INV-004 | Result由Invocation持有，take fault不会重复执行method。 |
| INV-005 | Endpoint close不取消已dispatch且仍有Responder的invocation。 |
| INV-006 | NOT_DELIVERED与OUTCOME_UNKNOWN可被native caller区分。 |
| INV-007 | 默认不自动retry；idempotency与业务operation ID语义明确。 |
| INV-008 | One-way notification不创建Invocation/Responder，且只能用于schema显式允许的best-effort method。 |
| IDL-001 | Protocol major使用UUID并在capability acquisition时协商revision/features。 |
| IDL-002 | Invocation payload不含protocol UUID、version、handle number或public txid。 |
| IDL-003 | 所有transport共享同一canonical value wire。 |
| IDL-004 | Handle ownership、scope和rights由schema声明。 |
| IDL-005 | Stable protocol必须由codegen与compatibility checker验证。 |
| SEC-001 | Driver/service implementation不接触user pointer或wire padding。 |
| SEC-002 | Structural protocol violation关闭对应endpoint。 |
| SEC-003 | Capability acquisition后authorization以scope/rights为准。 |
| SEC-004 | Remote broker完全在用户态，kernel不解释网络identity。 |
| DATA-001 | Native ABI不存在任意handle通用read/write。 |
| DATA-002 | 大块/高频数据使用MemoryObject、ring、Stream或Pager。 |
| MIG-001 | 每个migration phase可独立build、boot和验证。 |
| MIG-002 | 终态删除native fd/ioctl legacy syscalls与kernel VFS coupling。 |

## 25. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| Invocation/Responder object数量增加 | Kernel allocation和lookup成本 | Hard limits、slab/pool、测量后增加fast path，不改变ownership语义。 |
| Unified invoke隐藏transport差异 | Caller错误retry或忽略peer crash | Binding class可观察、分层CallError、remote有限budget。 |
| Capability view设计过细 | Handle数量和API复杂度上升 | Schema composition只用于真实需求；typed wrapper隐藏机械操作。 |
| Userland fd table增加lookup | POSIX I/O固定成本 | Lookup在userland；native API不承担；热点使用typed binding/cache。 |
| Unique endpoint限制多进程消费 | Server扩展方式变化 | Per-client connection + dispatcher/worker，Responder可MOVE。 |
| Canonical wire增加本地encode/decode成本 | 小调用延迟上升 | Bounded generated decoder、register/inline fast path后续优化。 |
| No general revocation | 错误delegation难回收 | 对敏感授权使用lease/proxy/gate；最小rights。 |
| No priority donation v1 | 高优先级client可能反转 | 系统service调度策略、deadline、trace；保留同机调用链。 |
| Per-process quota可被spawn绕过 | DoS隔离有限 | v1硬上限与service admission；后续ResourceDomain。 |
| Protocol UUID治理不完善 | 重复/孤儿schema | Repository registry、owner/stability review、CI collision check。 |
| Generated parser漏洞 | Kernel/service攻击面 | Negative tests、fuzz、bounded types、structural violation disconnect。 |
| In-transit endpoint形成ownership cycle | 无进程可达但kernel object永久滞留 | Rooted graph accounting、迭代reaper、cycle regression test；稳定transfer前必须解决。 |
| Service restart使旧handles失效 | Client恢复逻辑增加 | Service directory re-resolve；需要时显式stable proxy。 |
| Remote MOVE存在分布式歧义 | Authority丢失或重复 | 只把真实capability MOVE给本地broker；远端只得到proxy。 |
| Big-bang迁移导致系统不可启动 | 难以定位回归 | Legacy shim分阶段删除，每阶段boot gate。 |
| TTY/PTY文档仍以fd/ioctl描述 | 实现边界混乱 | 把其视为POSIX兼容需求，并按本PRD映射到native primitives。 |

## 26. 延后决定

以下事项不阻塞Phase 0–2，但必须在对应ABI稳定前确定：

1. 第一版handle table slot/generation内部编码与最大entry数。
2. v1 之后新增 native syscall 的版本化分配策略，以及后续 frame size/offset。
3. Single-message、per-endpoint和per-process hard limit具体数值。
4. `handle_get_info` 可公开的object diagnostics边界。
5. ProtocolDescriptor注册、共享、compact scope index和kernel缓存的具体ABI。
6. Composite protocol method ordinal分配细则。
7. Raw channel event与subscription的最终envelope flags。
8. Responder close后更细的reason taxonomy。
9. Operation budget在同机多跳调用中的传播API。
10. ResourceDomain/Job的层级配额和统一销毁语义。
11. 同机priority donation的最大深度与scheduler coupling。
12. MemoryObject/Pager page ownership、writeback和service crash策略。
13. Remote broker认证协议、lease默认值和session恢复策略。
14. Stable protocol UUID registry的review流程。
15. mlibc POSIX fork在多线程进程中的stop-the-world与handle snapshot实现。
16. Legacy syscall删除的确切release gate。
17. In-transit endpoint orphan graph的增量reaper、commit-time cycle prevention或混合实现选择。

## 27. 参考设计

- [Zircon handles：process-local handle、rights与kernel object lifecycle](https://fuchsia.dev/fuchsia-src/concepts/kernel/handles)
- [Zircon channel：bounded message、atomic handle transfer、unique endpoint ownership](https://fuchsia.dev/fuchsia-src/reference/kernel_objects/channel)
- [FIDL handle rights与resource transfer](https://fuchsia.dev/fuchsia-src/concepts/fidl/life-of-a-handle)
- [FIDL language与strict/flexible演进模型](https://fuchsia.dev/fuchsia-src/reference/fidl/language/language)
- [seL4 capabilities与CSpace](https://docs.sel4.systems/Tutorials/capabilities.html)
- [seL4 IPC与one-time reply capability](https://docs.sel4.systems/Tutorials/ipc)
- [gRPC retry：只有能证明server application未处理时才可透明retry](https://grpc.io/docs/guides/retry/)
- [Cap'n Proto RPC capability model](https://capnproto.org/rpc.html)
- POSIX.1-2024：[fork与shared open description](https://pubs.opengroup.org/onlinepubs/9799919799/functions/fork.html)、
  [dup/dup2](https://pubs.opengroup.org/onlinepubs/9799919799/functions/dup.html)
