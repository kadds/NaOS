# PRD：将文件系统迁移到用户态

- 状态：Draft
- 日期：2026-08-17
- 负责人：NaOS kernel / userland 维护者
- 关联：[总体架构](ARCHITECTURE.md)、[Capability 与 Invocation ADR](OBJECT_CALL_ADR.md)
- 本文对应基线：`a181c8f`；本文只定义产品与迁移契约，不实现代码。

## 1. 摘要

NaOS 的目录树、路径解析、inode/dentry、具体文件系统、挂载策略和 POSIX 文件描述符语义应迁移到用户态。
内核最终只保留进程/线程、地址空间、capability、IPC、内存对象、等待和硬件机制；它不再保存全局根目录、当前目录、`dentry`、`inode` 或文件系统驱动。

第一版交付一个早期系统用户态进程 `vfsd`：它以只读 boot archive 为初始存储，提供现有 `Directory` 与 `File` NaoIDL 协议的 user-service 实现，并启动 `/bin/init`。普通程序经 mlibc 的 POSIX 兼容层访问 `vfsd`，而非进入 kernel VFS。

这不是一次把块设备、可写持久文件系统和用户 pager 全部交付的项目。第一版使用内核 `MemoryObject` 作为有限、可验证的可执行文件和 `MAP_PRIVATE` 桥接；`MAP_SHARED`、写回、块设备、挂载和缺页 pager 明确留到后续阶段。

## 2. 现状与问题

### 2.1 现有实现事实

当前路径虽然已经有 native protocol 外观，但实际后端仍在内核：

- `idl/system/file.naidl` 与 `idl/system/directory.naidl` 已定义 `File`/`Directory`；mlibc 已把 `openat`、`read`、`write`、`stat`、目录操作等翻译为这些 invocation。
- `naos/src/kernel/ipc/invocation.cc` 的 `publish_file_call()` 与 `publish_directory_call()` 解码这些 invocation 后，直接调用 `fs::vfs::*` 和 `fs::vfs::file`。
- `naos/src/kernel/fs/vfs/` 持有全局根、路径解析、mount list、dentry/inode/file/superblock；`ramfs`、`rootfs`、`pipefs` 都在 kernel target。
- `rootfs.cc` 将 Multiboot `rfsimg` 的 TAR 条目解压进内核 `ramfs`；启动时 `idle_task.cc` 从内核 VFS 打开 `/bin/init`。
- `process_exec`/`process_spawn` 只接受 `NA_BINDING_KERNEL_VIEW + NA_SCOPE_FILE`；ELF loader 和 VM 的 file-fault path 都保存 `fs::vfs::file *`。因此仅把 POSIX `open()` 改为 IPC 不足以移除 VFS。
- kernel 当前还用 `native_directory` 记录进程 root/cwd，并由 `Directory.set_current`/`set_root` 改写；这与 ADR 中“namespace 是用户态 path context”的目标相冲突。
- 当前 `MemoryObject` 已能被内核映射，但没有用户态创建入口，且最大大小为 16 MiB；它还不能作为 executable。

### 2.2 要解决的问题

1. 将高故障面（TAR/未来 FAT、ext2、缓存、路径解析和 mount policy）从特权态移出。
2. 让 `File`/`Directory` 的同一 NaoIDL contract 可由 kernel adapter（过渡期）或 user service 实现，而 mlibc 不依赖 binding 为 `KERNEL_VIEW`。
3. 在移除 VFS 后仍可完成 cold boot、启动 `vfsd`、启动 `/bin/init`、`exec` 和受限的 `mmap`。
4. 不在 page fault 中同步执行任意 user service，也不以“临时保留一个隐藏 VFS”作为最终架构。

## 3. 目标、非目标与成功标准

### 3.1 目标

- `vfsd` 是 `/` 根目录、路径解析和 boot archive 中常规文件的唯一实现者；通过 ServiceDirectory 发布 `naos://system/filesystem` listener。
- mlibc 的普通文件操作仅保留 typed `Directory`/`File` client invocation；相同的代码可调用 `CLIENT_END`。
- kernel 以 boot module 启动 `vfsd`，将 `rfsimg` 以只读 `MemoryObject` capability 交给它；kernel 不解析 TAR，也不构造 rootfs dentry 树。
- `vfsd` 从 archive 取得 `/bin/init`，materialize 为只读 `MemoryObject`，通过新 executable ABI 启动 init，并在 bootstrap channel 中移交 root/cwd/ServiceDirectory/stdio。
- `exec` 和 `MAP_PRIVATE` 不再依赖 `fs::vfs::file`；内核只读取或映射已授权的 `MemoryObject`。
- 第一期完成后 kernel target 不再编译或包含 `kernel/fs/{vfs,ramfs,rootfs,pipefs}`，也不再保存 `native_directory`、`global_root` 或 kernel file-backed VM map。

### 3.2 非目标

- 第一版不支持块设备、分区、FAT/ext2、可写持久化、崩溃恢复、配额、加密或网络文件系统。
- 第一版不支持任意大小可执行文件、`MAP_SHARED`、`msync` 或用户 pager；单个 materialized object 不得超过发布时的 `NA_MEMORY_OBJECT_MAX_BYTES`。
- 第一版不承诺多用户认证语义。当前实现的 uid/gid/access 已基本等价于 root，不能在迁移中误称为已提供隔离。
- 不更改 capability / channel / Invocation 基础语义，不让 kernel 从路径名隐式发现服务，也不提供 service crash 的透明重连。
- `/dev/ptmx`、`/dev/tty*`、`/dev/console` 的 mlibc-to-`ttyd` 特例可在本期保留；其 namespace 化归入后续 DevFS 阶段。

### 3.3 可验收的结果

在 `python3 util/run.py q --iso -n` 的干净启动中：

1. kernel 日志显示 boot module 启动 `vfsd`，随后 `vfsd` 注册 filesystem listener 并启动 init；没有 `Root file system init`、`VFS init` 或 kernel TAR 解包路径。
2. init 和 BusyBox 能完成 `pwd`、`ls`、`cat /data/banner.txt`、`mkdir`、创建/读取/删除普通文件、`readlink`、`chdir`、`exec /bin/sh`。
3. 新启动的子进程从 bootstrap channel 获得 `CLIENT_END` root/cwd，而不是 `KERNEL_VIEW Directory`；`dup` 后共享 file offset 的行为维持现有 POSIX 兼容承诺。
4. 文件服务崩溃时，已持有 endpoint 的调用以 peer-closed/I/O error 失败，系统不发生 kernel panic、UAF 或无限缺页循环；不自动重绑旧 fd。
5. 无 VFS kernel source 后，kernel、`vfsd`、mlibc、init、BusyBox 均能编译，ISO 能启动并留下可核查 serial log。

## 4. 产品边界与总体架构

```text
GRUB modules
  ├── naos-vfsd         (独立 ELF boot module)
  └── rfsimg            (只读 TAR archive)
             │
             ▼
kernel: boot-module loader + IPC + capability + VM/MemoryObject
             │  boot archive MObj, service directory, bootstrap stream
             ▼
vfsd (early system user process)
  ├── namespace / path resolver / mount policy
  ├── boot TAR loader → user-space RAM backend (MVP)
  ├── File / Directory protocol server
  ├── archive file → immutable MemoryObject materializer
  └── launches /bin/init with a bootstrap channel
             │
             ▼
init / mlibc / BusyBox
  └── POSIX fd table → Directory/File ClientEnd → vfsd
```

### 4.1 长期 kernel / userland 划分

| 内核长期保留 | 迁移到用户态 |
| --- | --- |
| capability table、channel、Invocation/Responder、wait、process/thread、VM、MemoryObject、boot-module ELF loader、最小 bootstrap stream | 路径解析、目录树、inode、文件数据、TAR parser、权限策略、file offset/open description、具体 FS、namespace、mount 与 device namespace |

`MemoryObject` 是内核的数据平面对象，不是 filesystem。内核只依据 capability rights 映射/读取它，不能从对象反查路径、inode 或对应 `vfsd`。

### 4.2 `vfsd` 的特权边界

`vfsd` 是 kernel-adjacent 的早期系统服务，而不是内核组件：

- kernel 以固定 boot module 启动它，使它不依赖尚未存在的文件系统；它获得 boot archive、最小诊断 stream 与受限的 ServiceDirectory 注册权限。
- 它是 root namespace 和 mount policy 的系统策略 TCB；失效会导致根目录服务失效。MVP 将其视为受控系统级故障，不承诺透明重启。
- 它仍是普通用户态保护域：不链接 kernel target、不包含 `naos/includes/kernel`、没有直接页表/调度器/硬件访问权；它只能通过 capability 与 syscall 使用 kernel mechanism。
- `vfsd` 只需要获得能注册 `naos://system/filesystem` 的最小权限。后续 service manager 接管 namespace policy 时，应进一步从它剥离全局服务管理权。

### 4.3 绑定规则

- mlibc 接受 `NA_BINDING_CLIENT_END` 的 `File` 与 `Directory`；过渡内核 adapter 可继续使用 `KERNEL_VIEW`，但 mlibc 不得以 binding type 选择不同 POSIX 语义。
- `vfsd` 只能返回 File/Directory client endpoint，不能把 server endpoint、responder 或 kernel implementation pointer 传给客户端。
- `Directory` capability 表达一个服务端授权的 namespace root/current node；绝对/相对路径、`..`、symlink escape 和 chroot containment 均由它的服务端判定。
- root/cwd 是 mlibc runtime 的强引用。创建子进程时，父 runtime 显式将它们写入 bootstrap message；kernel 只原子转移资源，不解释目录对象。

## 5. MVP 设计

### 5.1 启动链

1. 构建系统把 `vfsd` 生成为单独的 Multiboot module，而不是只放进 `rfsimg`。`rfsimg` 继续是普通 root archive。
2. kernel 只验证和装载 `naos-vfsd` 的 ELF；它不在 kernel 中路径查找或解析 TAR。这个 bootstrap ELF loader 是受限、固定的 boot mechanism，不是 VFS。
3. kernel 将 archive 作为只读、可 map/read、不可 duplicate 给普通进程的 `MemoryObject` capability 交给 vfsd；当前 16 MiB 上限不足时，必须在实现前给 boot archive 定义单独的 immutable-boot-object backing 或提高且测试硬上限，不能静默截断。
4. vfsd 走“early service bootstrap”启动：拥有 boot archive、ServiceDirectory system-manager 权限和最小诊断 stream；此 bootstrap 模式没有 root/cwd，不能作为普通应用运行时。
5. vfsd 在用户态处理 archive，建立根 namespace，创建 `Directory`/`File` protocol descriptor 和 listener，发布 `naos://system/filesystem`，再经该 listener 获得自己的 root client endpoint。
6. vfsd 打开 archive 中 `/bin/init`，将其 materialize 为 immutable `MemoryObject`，创建 bootstrap channel，调用 process-spawn，并向子进程发送 root/cwd、ServiceDirectory 和 stdio。init 从此使用普通 mlibc bootstrap。

任何一步失败必须以明确的 boot error 写到诊断 stream 后停机或回到受控 failure；不得退回到 kernel rootfs。

### 5.2 为什么有 TAR，以及 MVP 是否需要 `tarfsd`

`tarfs` 不是长期架构所必需的文件系统。它出现的唯一原因是当前 `rfsimg` 由 `util/pack.py` 生成为 TAR：现有 kernel `rootfs.cc` 解析 TAR 并把它复制进 kernel ramfs。移除 kernel VFS 后，解析和转换这份 archive 的工作仍必须存在于用户态。

MVP **不要求独立的 `tarfsd` 进程**。推荐实现为 vfsd 的一次性启动步骤：它在用户态解析只读 boot archive，把普通文件、目录、hard link 和 symlink 解包到自己的 RAM backend；archive 随后只是启动输入。创建、写入、truncate、unlink、rename、link、symlink、mkdir 全部修改该用户态 RAM backend，重启后丢失。

之后可以按隔离与扩展需求拆分：

```text
vfsd (namespace / path / mount)
  └── tarfsd (optional read-only boot archive backend)
```

此时 `tarfsd` 只是可挂载的只读 lower layer，而非 VFS 的组成部分。等接入块设备后，它可被 `fatfsd`、`ext2fsd` 或其他具体 filesystem service 替换；这些服务可以按 mount instance 独立运行。无论是否拆分，跨挂载路径、`..`、symlink containment 与 mount policy 都留在 vfsd，具体 filesystem service 不拥有全局 namespace。

MVP RAM backend 仍必须：

- 解析 GNU TAR 所需的当前镜像条目、普通文件、目录、hard link 与 symlink；遇到不支持或畸形条目必须失败，不得越界读取。
- 维护自己的 inode ID、link count、metadata、open description 和目录 cursor。客户端不得依赖 inode 地址或 cursor 的位布局。
- 每个 `Directory.open` 返回一个独立的 File/Directory client endpoint；`dup` 由 mlibc 对同一 endpoint 的 capability 复制实现，服务端必须按 open description 共享 offset。
- 单个 read/write/request 的数据量不超过 `NA_CHANNEL_MAX_MESSAGE_BYTES`；mlibc 分段，vfsd 对超限返回规范错误而不是分配不受限缓冲。
- 初始 access policy 明确为单用户 uid/gid 0；协议响应保留现有 `Stat` 字段，但不得暗示多用户访问控制已经完成。

### 5.3 协议与 ABI 变化

现有 `File`/`Directory` schema 是迁移基础，但在首次发布前做以下受控演进：

1. 为 `File` 增加 `materialize`（名称可在 IDL review 中调整）：它返回一个 MOVE 的、只读 `MemoryObject` resource，带文件长度和 immutable generation。只允许 regular file，且拒绝超过上限的文件。
2. 新增 `MemoryObject` 创建 syscall/frame。创建者获得 read/write/map/transfer rights；vfsd 写完 executable 后以 `handle_restrict` 派生只读、只读-map 的对象并 MOVE 给调用者。禁止 vfsd 用 caller 提供的普通 MObj 作为 executable backing。
3. `na_process_exec_frame` 和 `na_process_spawn_frame` 的 executable 接受 `NA_BINDING_MEMORY_OBJECT + NA_SCOPE_MEMORY_OBJECT`；相应 process loader 接收 MObj，而不是 `fs::vfs::file`。`path` 只用于 argv/诊断名称，不能再次被 kernel 打开。
4. `Directory.set_current` 与 `set_root` 从普通 Directory contract 移除或改为兼容性 no-op；mlibc 更新自身 root/cwd handle，不允许 invocation 改写 `process_t` 的目录状态。
5. 新增 `NA_BOOTSTRAP_FLAG_EARLY_SERVICE` 及 `NA_BOOTSTRAP_CAPABILITY_BOOT_ARCHIVE`。常规 bootstrap 仍要求 root/cwd/ServiceDirectory/stdio；early service bootstrap 只向固定的初始 vfsd 发放上述最小能力。

所有 schema 修改必须由 NaoIDL 生成 bindings 与 ABI manifest；不可手写 method ordinal 或 canonical payload。

### 5.4 exec 与 mmap 桥接

MVP 不实现 pager：

- `File.materialize` 复制 snapshot 到内核 MObj。成功返回后，后续 vfsd 写入不影响该 object。
- kernel ELF loader 抽象为 “read executable object”和“map executable object”；ELF header、program header 与 load segment 都从 MObj 读取，VM file-fault path 改为 MObj fault path。
- mlibc 对 regular-file `MAP_PRIVATE` 先 materialize 再调用现有 memory-map object ABI；`MAP_SHARED`、`msync`、超限 mapping 返回 `ENOTSUP`/`EFBIG` 的确定错误。设备/terminal 保持各自的 protocol path。
- vfsd 与 kernel 之间没有由 page fault 触发的 synchronous invocation。pager、page cache 和 shared writeback 是独立的后续 PRD，不能偷偷塞入本期。

### 5.5 bootstrap 与 stdio

- `na_bootstrap` 的 channel 分支保留为唯一常规 child bootstrap：kernel 只校验 transferred resources 的 protocol metadata，绝不转换为 `native_directory`。
- fork/spawn 兼容层负责显式复制 root/cwd、fd binding 和 stdio endpoint；不得从 kernel process fields 回填。
- 现有 `/dev/kconsole` VFS pseudo 不能作为 bootstrap 依赖。kernel 要提供最小 `Stream` KernelView 作为 early diagnostic stream；ttyd 启动后仍按现有服务路径接管终端。

## 6. 分阶段计划与退出条件

### Phase 0：契约冻结与可测试接口

冻结本 PRD 的 MVP 范围、error mapping、early-service bootstrap 和 MObj executable 设计。补齐 NaoIDL schema、生成式 server skeleton 和 C++ protocol contract tests。

退出条件：schema 兼容性检查通过；测试可证明 File/Directory `CLIENT_END` 可被 mlibc binding 使用，而测试不依赖 kernel VFS 实现。

### Phase 1：消除 kernel file 对 exec/mmap 的依赖

实现用户可创建 MObj、MObj executable loader、MObj-backed `MAP_PRIVATE`；将 process exec/spawn、ELF loader、VM map 的 `fs::vfs::file *` 依赖改为 MObj。

退出条件：现有 kernel VFS 下仍能启动 init，且从 MObj 启动一个 ELF、读取多个 LOAD segment、私有映射、超限/无 rights 的负向路径均被 C++ tests 覆盖。

### Phase 2：vfsd 与 boot module

新增 `naos/src/usr/vfsd/`、用户态 TAR loader/RAM backend、Directory/File server、materialize，实现独立 Multiboot module 和 early bootstrap。vfsd 完成 listener 注册后从 archive 启动 init。独立 `tarfsd` 不属于此阶段退出条件。

退出条件：QEMU cold boot 由 vfsd 启动 init；archive 畸形、vfsd listener 失败、init 不存在、materialize 超限均产生受控失败。

### Phase 3：mlibc 切换与兼容验证

移除 mlibc 对 File/Directory 必为 kernel view 的假设；root/cwd 只保存在 runtime；将 spawn/exec 和 private file mmap 切换到 materialize。

退出条件：BusyBox 文件基本操作、relative/absolute/chroot path、symlink loop、open flags、fd offset sharing、fork/spawn bootstrap 在 QEMU 上通过。

### Phase 4：删除内核 VFS

删除 `naos/src/kernel/fs/{vfs,ramfs,rootfs,pipefs}` 及对应私有 headers，删除 `publish_file_call`/`publish_directory_call` adapter、`native_directory`、`global_root`、kernel TAR loader、kernel VFS pseudo device 初始化和 VFS file-backed VM path。更新架构文档与 build graph。

退出条件：kernel 不含这些源和头文件依赖；默认 ISO boot、日志、tty service 启动和 BusyBox smoke test 都通过；无 hidden fallback。

### Phase 5：后续独立项目（不阻塞 MVP）

定义 Block/Device、Namespace/Mount 和 Pager 协议，增加 DevFS、可写持久 filesystem、page cache、`MAP_SHARED`/writeback、service restart policy 与多用户凭据。每一项另立 PRD 和 ABI review。

## 7. 错误、恢复和安全要求

- 服务端 protocol/domain error 使用现有负 errno；transport failure 保留 `NOT_DELIVERED` 与 `OUTCOME_UNKNOWN`，mlibc 映射为确定的 POSIX error。写入、create、rename 等非幂等操作不得对 `OUTCOME_UNKNOWN` 自动重试。
- vfsd crash 后已发出的 client endpoint 必须 `PEER_CLOSED`；新的服务实例只可通过显式 ServiceDirectory connect 获得，旧 fd 不得静默改绑。
- boot archive 只授予 vfsd；init 和一般进程没有该 capability。vfsd 获得的 ServiceDirectory 权限仅允许 `naos://system/filesystem` 注册，或由独立 service manager 进一步缩小。
- 所有 archive offset/length、path length、symlink depth、directory cursor、channel bytes/resources 和 MObj size 必须做溢出及硬上限校验。
- kernel 不信任 vfsd 的 `Stat`、file type 或 executable 名称；它只验证 MemoryObject capability、rights、范围和 ELF bytes。ELF 无效只终止本次 exec/spawn，不破坏调用者现有地址空间。

## 8. 验证矩阵

| 层级 | 必测场景 |
| --- | --- |
| NaoIDL / client-server | File/Directory request/response、resource metadata、错误/peer close、64 KiB bound、畸形 payload |
| vfsd | TAR 边界/overflow、普通文件/目录/link/symlink、RAM backend 语义、open offset、rename/unlink、materialize immutable snapshot |
| kernel | MObj rights、exec 的 ELF segment load、private mmap、bootstrap resource transaction、vfsd crash 后 capability/waiter cleanup |
| mlibc | `openat`、`readv/writev`、`getdents`、`chdir/chroot/getcwd`、`dup` offset sharing、spawn/exec、错误映射 |
| QEMU | cold boot、BusyBox smoke、文件服务故障、超限 executable、serial log 中无 VFS fallback |

遵从仓库测试规则：行为用 C++ tests、真实编译和 QEMU 验证；不添加用 Python 扫描/解析 C++ 源码来“证明”迁移完成的测试。

## 9. 实施风险与决策记录

| 风险 | 决策 / 缓解 |
| --- | --- |
| 启动鸡生蛋 | `vfsd` 独立 boot module；archive 作为 capability，不从 kernel VFS 打开 vfsd。 |
| exec 重新依赖 VFS | 先落地 MObj executable，再删除 VFS。 |
| page fault deadlock | MVP 禁止 user pager；只 map kernel MObj。 |
| 16 MiB MObj 上限 | 在 build/boot gate 中检查 archive/ELF；超限必须显式失败或先实现专用 immutable boot backing。 |
| 在 kernel 留下隐式 root/cwd | Phase 4 删除 `native_directory` 与 process 目录字段；bootstrap 只转发 capability。 |
| File/Directory 语义漂移 | 将 endpoint contract tests 同时跑在 temporary kernel adapter 与 vfsd server 上，直到 adapter 删除。 |
| TAR 被误当成长期开机依赖 | MVP 只把 TAR 视为当前镜像格式的用户态导入器；`tarfsd` 是可选拆分，不阻塞后续 FAT/ext2 等 backend。 |
| `/dev` path 仍在 mlibc 特判 | 明确标为过渡债务，后续由 DevFS 统一；本期不把它伪装成 kernel VFS。 |

## 10. 首个实现切片

建议第一张实现 issue 只做 Phase 1 的最小纵切：新增 MObj 创建与 executable frame 支持，将一个由 kernel test 创建并填充的 ELF MObj 用于 process spawn；暂不接入 vfsd 或改动 rootfs。它把最大架构风险（loader/VM 仍绑 VFS）先拆掉，并为随后可回滚的 vfsd 接入提供稳定边界。
