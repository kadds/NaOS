# ADR：将文件系统迁移到用户态

- 状态：Accepted
- 日期：2026-08-17
- 负责人：NaOS kernel / userland 维护者
- 关联：[总体架构](ARCHITECTURE.md)、[Capability 与 Invocation ADR](OBJECT_CALL_ADR.md)
- 本文对应基线：2026-09-12 当前实现；本文记录用户态文件系统迁移的架构决策、边界和验证要求。

## 1. 摘要

NaOS 的目录树、路径解析、inode/dentry、具体文件系统、挂载策略和 POSIX 文件描述符语义应迁移到用户态。
内核最终只保留进程/线程、地址空间、capability、IPC、内存对象、等待和硬件机制；它不再保存全局根目录、当前目录、`dentry`、`inode` 或文件系统驱动。

第一版交付一个早期系统用户态进程 `vfsd`：它以只读 boot archive 为初始存储，提供现有 `Directory` 与 `File` NaoIDL 协议的 user-service 实现，并启动 `/bin/init`。普通程序经 mlibc 的 POSIX 兼容层访问 `vfsd`，而非进入 kernel VFS。

这不是一次把块设备、可写持久文件系统和通用用户 pager 全部交付的项目。当前可执行文件和 regular-file `MAP_PRIVATE` 使用有限、可验证的 `MemoryObject` + `File` pager 桥接；`MAP_SHARED`、写回、块设备、挂载和通用 pager 仍留到后续阶段。

## 2. 现状与问题

### 2.1 现有实现事实

当前路径虽然已经有 native protocol 外观，但实际后端仍在内核：

- `idl/system/file.naidl` 与 `idl/system/directory.naidl` 已定义 `File`/`Directory`；mlibc 已把 `openat`、`read`、`write`、`stat`、目录操作等翻译为这些 invocation。
- `naos/src/kernel/ipc/invocation.cc` 的 `publish_file_call()` 与 `publish_directory_call()` 解码这些 invocation 后，直接调用 `fs::vfs::*` 和 `fs::vfs::file`。
- `naos/src/kernel/fs/vfs/` 持有全局根、路径解析、mount list、dentry/inode/file/superblock；`ramfs`、`rootfs`、`pipefs` 都在 kernel target。
- 旧的内核 rootfs/TAR 启动路径已删除；`idle_task.cc` 只发布命名 boot
  module 的 MemoryObject 并启动用户态 filesystem worker。
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

- `vfsd` 是 `/` 根目录、路径解析和 boot archive 中常规文件的唯一实现者；通过 ServiceDirectory 发布 `naos://service/fs/vfs/0` listener。
- mlibc 的普通文件操作仅保留 typed `Directory`/`File` client invocation；相同的代码可调用 `CLIENT_END`。
- kernel 以 boot module 启动 `vfsd`，并将 `rootfsd`、`init` 和
  `rootimage` 以只读 `MemoryObject` 发布为一次性 ServiceDirectory 资源；
  kernel 不解析文件系统格式，也不构造 rootfs dentry 树。
- `vfsd` 从 `rootimage` 取得 mounted backend，待 worker commit 后通过新
  executable ABI 启动 init，并在 bootstrap channel 中移交
  root/cwd/ServiceDirectory/stdio。
- `exec` 和 `MAP_PRIVATE` 不再依赖 `fs::vfs::file`；内核只读取或映射已授权的 `MemoryObject`。
- 第一期完成后 kernel target 不再编译或包含 `kernel/fs/{vfs,ramfs,rootfs,pipefs}`，也不再保存 `native_directory`、`global_root` 或 kernel file-backed VM map。

### 3.2 非目标

- 第一版不支持块设备、分区、FAT/ext2、可写持久化、崩溃恢复、配额、加密或网络文件系统。
- 第一版不支持任意大小可执行文件、`MAP_SHARED`、`msync` 或通用用户 pager；File-backed executable object 仍不得超过 `NA_MEMORY_OBJECT_MAX_BYTES`。
- 第一版不承诺多用户认证语义。当前实现的 uid/gid/access 已基本等价于 root，不能在迁移中误称为已提供隔离。
- 不更改 capability / channel / Invocation 基础语义，不让 kernel 从路径名隐式发现服务，也不提供 service crash 的透明重连。
- `/dev/ptmx`、`/dev/tty*`、`/dev/console` 的 mlibc-to-`ttyd` 特例可在本期保留；其 namespace 化归入后续 DevFS 阶段。

### 3.3 可验收的结果

在 `python3 util/run.py --build-dir build-debug q --iso -n` 的干净启动中：

1. kernel 日志显示 boot module 启动 `vfsd`，随后 `vfsd` 注册 filesystem listener 并启动 init；没有 `Root file system init`、`VFS init` 或 kernel TAR 解包路径。
2. init 和 BusyBox 能完成 `pwd`、`ls`、`cat /data/banner.txt`、`mkdir`、创建/读取/删除普通文件、`readlink`、`chdir`、`exec /bin/sh`。
3. 新启动的子进程从 bootstrap channel 获得 `CLIENT_END` root/cwd，而不是 `KERNEL_VIEW Directory`；`dup` 后共享 file offset 的行为维持现有 POSIX 兼容承诺。
4. 文件服务崩溃时，已持有 endpoint 的调用以 peer-closed/I/O error 失败，系统不发生 kernel panic、UAF 或无限缺页循环；不自动重绑旧 fd。
5. 无 VFS kernel source 后，kernel、`vfsd`、mlibc、init、BusyBox 均能编译，ISO 能启动并留下可核查 serial log。

## 4. 产品边界与总体架构

```text
GRUB modules
  ├── vfsd              (独立 ELF boot module)
  ├── rootimage         (prepared FAT16 image, 8 KiB clusters)
             │
             ▼
kernel: boot-module loader + IPC + capability + VM/MemoryObject
             │  rootdir service, service directory, bootstrap stream
             ▼
vfsd (early system user process)
  ├── namespace / path resolver / mount policy
  ├── rootimage → user-space block/filesystem workers
  ├── File / Directory protocol server
  ├── archive file → sized MemoryObject + File pager
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

- kernel 以固定 boot module 启动它，使它不依赖尚未存在的文件系统；它获得最小诊断 stream 与受限的 ServiceDirectory 注册权限，并通过 boot-module resources 启动 worker。
- 它是 root namespace 和 mount policy 的系统策略 TCB；失效会导致根目录服务失效。MVP 将其视为受控系统级故障，不承诺透明重启。
- 它仍是普通用户态保护域：不链接 kernel target、不包含 `naos/includes/kernel`、没有直接页表/调度器/硬件访问权；它只能通过 capability 与 syscall 使用 kernel mechanism。
- `vfsd` 只需要获得能注册 `naos://service/fs/vfs/0` 的最小权限。后续 service manager 接管 namespace policy 时，应进一步从它剥离全局服务管理权。

### 4.3 绑定规则

- mlibc 接受 `NA_BINDING_CLIENT_END` 的 `File` 与 `Directory`；过渡内核 adapter 可继续使用 `KERNEL_VIEW`，但 mlibc 不得以 binding type 选择不同 POSIX 语义。
- `vfsd` 只能返回 File/Directory client endpoint，不能把 server endpoint、responder 或 kernel implementation pointer 传给客户端。
- `Directory` capability 表达一个服务端授权的 namespace root/current node；绝对/相对路径、`..` 和 symlink 解析均由它的服务端判定。chroot 属于传统路径视图而非安全边界：它只约束其后经 binding 进行的路径解析，既有 capability 仍是有效授权（[OBJECT_CALL_ADR](OBJECT_CALL_ADR.md) §9.7）；更强的 capability 隔离需另立 PRD。
- root/cwd 是 mlibc runtime 的强引用。创建子进程时，父 runtime 显式将它们写入 bootstrap message；kernel 只原子转移资源，不解释目录对象。

## 5. MVP 设计

### 5.1 启动链

1. 构建系统把 `vfsd`、`blockd`、`rootfsd`、`init` 和 `rootimage`
   生成为固定命名的 Multiboot modules。
2. kernel 只验证和装载 `vfsd` 的 ELF；它不在 kernel 中路径查找或解析 TAR。这个 bootstrap ELF loader 是受限、固定的 boot mechanism，不是 VFS。
3. kernel 将 `rootfsd`、`init` 和 `rootimage` 作为只读、可 map/read、
   不可 duplicate 给普通进程的 `MemoryObject` 发布；vfsd 通过
   ServiceDirectory 一次性查询这些资源。
4. vfsd 走“early service bootstrap”启动：拥有 ServiceDirectory
   system-manager 权限和最小诊断 stream，并在启动后查询 boot modules；此
   bootstrap 模式没有 root/cwd，不能作为普通应用运行时。
5. vfsd 在用户态建立根 namespace，创建 `Directory`/`File` protocol
   descriptor 和 listener，启动 rootfsd，等待 mounted route commit 后发布
   `naos://service/fs/vfs/0`。
6. vfsd 从独立 `init` boot module 创建 bootstrap channel，调用
   process-spawn，并向子进程发送 root/cwd、ServiceDirectory 和 stdio。

任何一步失败必须以明确的 boot error 写到诊断 stream 后停机或回到受控 failure；不得退回到 kernel rootfs。

### 5.2 为什么有 TAR，以及 MVP 是否需要 `tarfsd`

当前启动输入是 prepared FAT16 `root.img`（8 KiB clusters），由 ramdiskd 作为 block backend
提供给 exfatd；不再存在 TAR archive 或独立 archive parser 的启动路径。
创建、写入、truncate、unlink、rename、link、symlink、mkdir 由 filesystem
worker 负责，并按 mounted namespace 的 MutationTicket 约束 metadata 变更。

之后可以按隔离与扩展需求拆分：

```text
vfsd (namespace / path / mount)
  └── tarfsd (optional read-only boot archive backend)
```

此时 `tarfsd` 只是可挂载的只读 lower layer，而非 VFS 的组成部分。等接入块设备后，它可被 `fatfsd`、`ext2fsd` 或其他具体 filesystem service 替换；这些服务可以按 mount instance 独立运行。无论是否拆分，跨挂载路径、`..`、symlink containment 与 mount policy 都留在 vfsd，具体 filesystem service 不拥有全局 namespace。

MVP RAM backend 仍必须：

- 解析 GNU TAR 所需的当前镜像条目、普通文件、目录、hard link 与 symlink；遇到不支持或畸形条目必须失败，不得越界读取。
- 维护自己的 inode ID、link count、metadata、open description 和目录 cursor。客户端不得依赖 inode 地址或 cursor 的位布局。
- 每个 `Directory.open` 返回一个独立的 File/Directory client endpoint；同进程 `dup` 由 mlibc 的运行时引用计数共享同一 native handle 实现，不得对 Directory/File 调用 `handle_duplicate`；fork/spawn 由服务端 `clone_binding` 提供新的 unique endpoint。服务端必须按 open description 共享 offset/游标。
- 单个 read/write/request 的数据量不超过 `NA_CHANNEL_MAX_MESSAGE_BYTES`；mlibc 分段，vfsd 对超限返回规范错误而不是分配不受限缓冲。
- 初始 access policy 明确为单用户 uid/gid 0；协议响应保留现有 `Stat` 字段，但不得暗示多用户访问控制已经完成。

### 5.3 协议与 ABI 变化

现有 `File`/`Directory` schema 是迁移基础，但在首次发布前做以下受控演进：

1. 为 `File` 增加 `materialize`：`File` 现为 revision 3（方法至 `writev @id(16)`），`materialize` 以 **revision 4 / `@id(17)`** 冻结——请求为空，响应含 MOVE 的只读 `handle<memory_object>`、`u64 length`、`u64 generation` 三个字段。只允许 regular file（否则 `EISDIR`），且拒绝超过上限的文件（`EFBIG`）。不引入新 named right，与 File 现行 read/write 同为 INVOKE-only 面。
2. 新增 `MemoryObject` 创建 syscall/frame。创建者获得 read/write/map/transfer rights；vfsd 写完 executable 后以 `handle_restrict` 派生只读、只读-map 的对象并 MOVE 给调用者。禁止 vfsd 用 caller 提供的普通 MObj 作为 executable backing。
3. `na_process_exec_frame` 和 `na_process_spawn_frame` 的 executable 接受 `NA_BINDING_MEMORY_OBJECT + NA_SCOPE_MEMORY_OBJECT`；相应 process loader 接收 MObj，而不是 `fs::vfs::file`。`path` 只用于 argv/诊断名称，不能再次被 kernel 打开。
4. `Directory.set_current` 与 `set_root` 保留为兼容性 no-op（user-service endpoint 固定返回 `ENOTSUP`，method id 12/13 冻结保留）；mlibc 更新自身 root/cwd binding，不允许 invocation 改写 `process_t` 的目录状态。`chroot` 通过 `Directory.open(..., NA_DIRECTORY_OPEN_FLAG_CHROOT)` 向 vfsd 请求一个以目标目录为 visible root/current 的受限 Directory endpoint；同进程 root/cwd 共享该 binding，fork/spawn 通过协议级 `Directory.clone_binding` 取得新的 unique endpoint。
5. 新增 `NA_BOOTSTRAP_FLAG_EARLY_SERVICE`。常规 bootstrap 仅要求 root/cwd/ServiceDirectory/stdio；early service bootstrap 复用 ServiceDirectory 与 STDIN/STDOUT/STDERR 资源槽（root/cwd 槽缺省），vfsd 通过 `naos://service/rootdir/0` 查询 boot archive，诊断流即 stdout/stderr，kernel 校验的最小资源数在该 flag 下按分支生效。
6. **Directory revision 2：POSIX 接口完备性**（与 `clone_binding @id(14)` 同一 revision 冻结）。补齐四组缺口：无跟随查询、目录 fsync、双 dirfd 原子 rename/link。各方法不引入新 named right，沿用 Directory 现行 INVOKE-only 面（与既有 rename/link/remove 同级，possession-is-authority）；`stat_node` 对不存在的路径返回 `ENOENT`；`stat_node` 的 NOFOLLOW、`rename_at` 的 NOREPLACE/EXCHANGE 预留位、`link_at` 的 FOLLOW 位随 schema 同步定名 UAPI 宏；末尾的 `reserved @uapi_name("DIRECTORY_SET_METADATA") @id(19)` 为未来 metadata 类操作（chmod/utimens 方向）保留 ordinal 防止复用（名称持久化进 manifest 并生成占位宏），Phase 0 落 schema 时以生成器支持的保留形式表达：

   ```naidl
   method stat_node @id(15) @max_bytes(65536) {
       u64 flags @id(1);            // bit0 = NA_DIRECTORY_LOOKUP_FLAG_NOFOLLOW
       u64 path_size @id(2);
       bytes<4095> path @id(3) @inline @length_field("path_size");
   } -> {
       Stat value @id(1);
   };
   method sync @id(16) ->;
   method rename_at @id(17) @max_bytes(65536) {
       u64 flags @id(1);            // v1 只接受零；NOREPLACE/EXCHANGE 位预留
       client_end<directory, transfer, move> new_parent @id(2);
       u64 first_size @id(3);
       u64 second_size @id(4);
       bytes<4095> first @id(5) @inline @length_field("first_size");
       bytes<4095> second @id(6) @inline @length_field("second_size");
   } ->;
   method link_at @id(18) @max_bytes(65536) {
       u64 flags @id(1);            // bit0 = AT_SYMLINK_FOLLOW 语义
       client_end<directory, transfer, move> new_parent @id(2);
       u64 first_size @id(3);
       u64 second_size @id(4);
       bytes<4095> first @id(5) @inline @length_field("first_size");
       bytes<4095> second @id(6) @inline @length_field("second_size");
   } ->;
   reserved @uapi_name("DIRECTORY_SET_METADATA") @id(19);
   ```

   语义固定如下：

   - `stat_node` 是路径形式查询，替代 mlibc 现行 open+stat 模拟。路径字段 `bytes<4095>` 为无 NUL 显式长度；现存 `open`/`create` 的 `bytes<4096>` 为 NUL 结尾 C 字符串（有效至 4094 字符），两者容量兼容，`stat_node` 覆盖全部可 open 路径。`NOFOLLOW` 只作用于最后一个 component（POSIX lstat 语义）：中间 component 始终跟随且必须为目录，否则 `ENOTDIR`；悬空 symlink 在 NOFOLLOW 下返回其自身 Stat（type LNK），不报 ENOENT；symlink 循环经全局 walk 预算返回 `ELOOP`。`NOFOLLOW` 不改变 mount 穿越：最终 component 命中 mountpoint 时仍进入被挂载根并返回其 Stat。
   - `sync` 即 `fsync(dirfd)`：持久 backend 在返回前必须使该目录的全部 entry/metadata 变更在其事务边界内持久化（接入 [VFS ADR](VFS_BLOCK_DEVICE_ADR.md) backend 后最终落到 `BlockDevice.flush` 排序域，“worker/metadata transaction”概念见该 ADR）；RAM backend 返回成功但明示不持久。`File.sync` 语义不变。
   - `rename_at`/`link_at` 的 `new_parent` 以 **MOVE disposition** 随消息转移：mlibc 先对该 dirfd 调用 `clone_binding` 生成临时副本、MOVE 副本进请求，服务端使用后即弃，调用者的原 handle 全程保留。MOVE 不触碰 ADR §11.2/§12.3 对 endpoint 复制权的限制，dirfd 也无需携带 meta DUPLICATE right；每次调用多出的一次 clone RPC 是 v1 有意接受的成本。
   - vfsd 对已签发的 Directory/NamespaceBinding client end 必须按 `handle_get_info` 返回的稳定 capability identity 登记，不能把进程内 raw handle number 当作身份；MOVE 到另一进程后 raw handle 会重新编号，但 capability identity 必须保持不变，才能正确解析 `new_parent` 并保留 `EXDEV` 的权威判断。
   - 服务端权威校验顺序：flags 合法性 → 两端 filesystem instance 一致性（以各自 `Stat.device` 的 mount 派生伪 st_dev 为准，见 [VFS ADR](VFS_BLOCK_DEVICE_ADR.md) §6.2）→ 不一致立即 `EXDEV`，无任何副作用。同 instance 时按 [VFS ADR](VFS_BLOCK_DEVICE_ADR.md) §6.4 的 `begin_mutation` 四节点加组件名预留执行单事务原子提交；`rename_at` 覆盖受害者按 POSIX 语义替换，`link_at` 目标已存在返回 `EEXIST`、FAT32/exFAT 按 [VFS ADR](VFS_BLOCK_DEVICE_ADR.md) 附录 A 返回 `EOPNOTSUPP`。
   - mlibc 与 Rust std 双侧同步落地：`stat`/`lstat`/`fstatat(AT_SYMLINK_NOFOLLOW)` → `stat_node`；`fsync(dirfd)` 按句柄 scope 分派到 `Directory.sync`/`File.sync`；`renameat`/`linkat` 删除现行“两个 dirfd handle 不相等即 EXDEV”的捷径，改为同一 handle 快路径或 `rename_at`/`link_at`。Rust 侧映射：`fs::metadata`/`fs::symlink_metadata` → `stat_node`(follow/NOFOLLOW)，`fs::rename` → `rename_at`，`fs::hard_link` → `link_at`，`File::sync_all/sync_data` → `File.sync`。

7. 新协议方法必须在同一变更中同时产出 C++（mlibc sysdeps）与 Rust（naos-idl 生成 binding + std PAL wiring）两侧客户端；生成 manifest 是错误映射的唯一事实源，两侧 io/errno 映射表随 revision 冻结。任何一侧缺失即视为 schema 未冻结；contract tests 必须同时覆盖两条通道，不得让任一侧成为唯一诊断来源。

所有 schema 修改必须由 NaoIDL 生成 bindings 与 ABI manifest；不可手写 method ordinal 或 canonical payload。

### 5.4 exec 与 mmap 桥接

当前实现使用受限的 File pager：

- mlibc 先通过 `File.stat` 取得长度，创建只读 `MemoryObject`，再把同一个 `File` client capability 作为 pager 传入现有 map/exec ABI；不会在启动阶段 materialize 整个 ELF。
- 缺页以 16 KiB 对齐窗口生成一次 `File.pread` invocation（通常覆盖四个 page，文件尾部按实际长度缩短）。`pread` 的数据通过一个可写 `MemoryObject` resource 直接填充最终物理页，响应只返回状态和 count；kernel pager worker 将窗口内的页安装到 MemoryObject page cache，不建立临时 payload 拷贝。
- page fault 的 exception/interrupt path 只登记 fault、阻塞当前线程并返回；实际 IPC 在 kernel pager worker 中完成，避免在 fault handler 内同步调用用户态服务。
- ELF header、program header 与 load segment 仍从 MObj 读取，`MAP_SHARED`、`msync`、writeback、通用 pager 和 generation-pinned snapshot 仍是后续工作。设备/terminal 保持各自的 protocol path。

### 5.5 bootstrap 与 stdio

- `na_bootstrap` 的 channel 分支保留为唯一常规 child bootstrap：kernel 只校验 transferred resources 的 protocol metadata，绝不转换为 `native_directory`。
- fork/spawn 兼容层负责显式复制 root/cwd、fd binding 和 stdio endpoint；不得从 kernel process fields 回填。
- 现有 `/dev/kconsole` VFS pseudo 不能作为 bootstrap 依赖。kernel 要提供最小 `Stream` KernelView 作为 early diagnostic stream；ttyd 启动后仍按现有服务路径接管终端。

### 5.6 跨平台 daemon transport 与大数据面

`ramdiskd`、`vfsd` 和 FAT-family worker（例如 `exfatd`）只维护一套业务源码和一套 NaoIDL contract；Linux 与 NaOS 的差异收敛在 `RpcTransport`、`MemoryObject` data-plane 和 bootstrap/runtime adapter。不得为了平台复制 `ramdiskd_linux`/`ramdiskd_naos` 等两套 daemon 实现。

```text
daemon service state machine / lease / VFS / FAT semantics
                         │
                         ▼
                   RpcTransport
                 ┌───────────────┬────────────────────┐
                 │ Linux         │ NaOS               │
                 │ UDS or queue  │ kernel Channel      │
                 │ std + Tokio   │ NaoIDL + naos_std   │
                 └───────────────┴────────────────────┘
```

#### 5.6.1 IDL 与 transport 分层

- NaoIDL generator 必须把纯 wire codec（字段、method id、UUID/revision、domain error、资源描述）与 transport glue 分开。codec 不能直接依赖 `naos_sys`、Unix fd 或某个 kernel handle 类型。
- Rust bindings expose the codec and transport-generic client with
  `--no-default-features --features alloc`; NaOS endpoint/resource glue is
  enabled separately by the `naos` feature. This keeps the Linux codec path
  free of syscall and fd types while preserving the native binding used by
  the early-service runtime.
- 生成的 client/server 以 `RpcTransport` 为参数；service handler 只依赖平台无关的 async trait/Future、`Endpoint`、`CallError` 和 `ResourceSet`。`RpcTransport` 是区分 Linux/NaOS 的唯一系统边界，daemon 的 lease、路径、FAT 和错误语义不得出现 target-specific 分支。
- Linux 版本使用正常 `std` 和 Tokio runtime。控制面使用本机 UDS（优先 `SOCK_SEQPACKET`；stream 方案必须使用长度前缀），或可替换的 Linux queue transport（POSIX mqueue，或 eventfd + shared-memory ring）。这些接口只允许本机进程 IPC，不引入 TCP/UDP/HTTP/DNS 或网络文件系统逻辑。
- ServiceDirectory 使用统一的层级 URI：文件系统服务位于 `naos://service/fs/...`，块设备服务位于 `naos://service/block/...`；当前 host slice 的三个实例是 `naos://service/fs/vfs/0`、`naos://service/fs/exfat/0` 和 `naos://service/block/ramdiskd/0`。`list_prefix` 按 URI segment 边界分页返回匹配的所有实例，不能把 bulk 注册 sidecar 当成服务。
- NaOS 版本使用 Rust fork 提供的 `std`/`naos_runtime`；Tokio 通过 NaOS 专用 Mio selector/waker 将 capability handle 的 epoll readiness 接入统一 runtime selector，transport 仍通过 Kernel Channel、NaoIDL invocation 和 capability MOVE/DUP 实现。Linux 的 opaque resource id 只模拟 capability contract，不宣称具有 kernel capability 的安全强度。
- service 的入口仍是同一个 crate：Linux 和 NaOS 都使用普通 Rust `fn main()`；NaOS 的
  `_start`、TLS、bootstrap 和 `std` 初始化由 `naos-runtime`/custom `std` 完成，业务入口只
  通过 servicekit 读取已准备好的 bootstrap 上下文。仅平台适配层允许
  `cfg(target_os = "naos")`。

#### 5.6.2 Bulk buffer contract

大段读写不得反复编码为 inline `bytes<...>`。控制消息与数据区域分离：

```text
control frame: request id, method, region id, offset, length, direction
bulk region:   shared bytes, owned until invocation completion/cancellation
```

- `BulkBuffer` 至少包含 `region_id`、`offset`、`length` 和 `direction`（in/out/inout）；所有算术、对齐、范围、generation 和 rights 检查均在 transport admission 与 backend 两侧执行。
- 小于等于 4 KiB 的控制数据可 inline；大于 4 KiB 的 File/Block 数据必须走 bulk。BlockDevice 的现有 `MemoryObject + buffer_offset + block_count` 已是该 contract 的 NaOS 形式，默认走 bulk；`File.read/write/readv/writev` 与 `MemoryObject.read/write` 后续 revision 必须提供等价的 bulk resource 形式。
- Linux UDS transport 在连接建立时通过 `SCM_RIGHTS` 传递 `memfd`/共享 ring FD，后续 invocation 只传 descriptor；Linux queue transport 只传 descriptor，数据放在同一共享 ring。不能把每个 64 KiB 请求重新复制进消息队列。
- NaOS transport 将 bulk region 映射为 `MemoryObject` capability，并按读写方向削减 rights；完成或取消前不得回收 region。NaOS kernel 继续拥有最终的 mapping、范围和 in-flight 校验。
- ring slot 数量必须受 `max_in_flight`、`max_transfer_bytes` 和 global pinned/bulk quota 约束；slot 耗尽返回 `WOULD_BLOCK`，不能无界分配或在 async handler 中同步等待可用 slot。
- `write` 在 producer 交出 region 后才允许 consumer 读取；`read` 在 consumer 填充 region 并完成 invocation 后才归还所有权。取消、peer close 和 transport failure 必须可回收 slot，非幂等 mutation 不得因为 `OUTCOME_UNKNOWN` 自动重试。

#### 5.6.3 Tokio 与同步 filesystem worker

- Linux daemon 的 RPC accept、request dispatch、timeout、cancellation、bounded queue 和 process supervision 使用 Tokio；不同 endpoint 的执行模型仍按 NaoIDL descriptor 保持串行或 `@concurrent`。
- 当前 FAT worker 的核心 API 是同步的，所有可能阻塞的 FAT metadata/data 操作必须进入 `tokio::task::spawn_blocking` 或专用 blocking pool；不得在 Tokio worker thread 直接执行未界定时延的 block/file I/O。
- NaOS service core 只暴露 async trait/Future，不把 `tokio::net` 或 `tokio::fs` 写进共享层。当前 NaOS adapter 已提供 Tokio 的 NaOS Mio capability selector/waker、timer 和通用 `WaitSet`；服务业务仍通过 servicekit 的 runtime facade 接入，不能在共享层伪造 UDS。

#### 5.6.4 Linux-first 验收

Linux 版本先在无 QEMU 环境完成三个独立进程的基础链路：`ramdiskd` 提供 memory backend，`exfatd` 通过 `RpcTransport` 从用户态 block manager 获取 block lease，`vfsd` 发布 mount/data endpoint。验收必须覆盖：

当前实现已经搭建并通过这条 Linux host slice 的基础链路：三个 binary 通过固定 service-root 下的 Tokio UDS 互相发现和调用，数据块通过 `SCM_RIGHTS` 注册的 bulk memfd 传输；host smoke 已覆盖 `/data` 的基本读写、fsync、rename、O_EXCL 和 active-file unlink。该 slice 与 NaOS 的 native bootstrap/mount transaction 是两条 adapter 路径，不能把 Linux opaque resource id、host proxy 或内存盘重启丢失误报为完整 NaOS VFS 实现；本节列出的 lease cancellation/backpressure 完整矩阵仍需单独补齐。

1. UDS（以及可选 queue transport）上的 IDL round-trip、revision/error/resource validation 和 peer-close；
2. 4 KiB 边界、64 KiB 最大传输和至少一个更大的 bulk region；数据面不得退化成 inline 全量复制；
3. 用户态 block manager 的只读拒绝、重叠 `EBUSY`、flush ordering、取消和 backpressure；
4. exfatd 的 format、read/write、fsync、truncate、rename、`O_EXCL` 与 active-file unlink；
5. vfsd `/data` 路由和服务重启/持久化（memory backend 明确不跨重启持久）；
6. `cargo test --workspace`、Tokio integration tests 与独立 Linux 进程 smoke 全部通过；平台运行时由 `servicekit` 按 target 自动选择。

## 6. 实施范围与验证

### Phase 0：契约冻结与可测试接口

冻结本 ADR 的 MVP 范围、error mapping、early-service bootstrap 和 MObj executable 设计。补齐 NaoIDL schema——含 Directory revision 2 全部方法与 File `materialize` 的 schema 冻结及 C++/Rust 双侧 binding 生成（实现落地分别在后续 phase）——生成式 server skeleton 和 C++ protocol contract tests。

> 依赖注记（r5）：本 phase 及 Phase 2 的 Rust server binding 依赖 [VFS ADR](VFS_BLOCK_DEVICE_ADR.md) §10 定义的 generator/Rust-server 前置交付；该 ADR 的前置条件已按阶段分解，其 Phase 0 与本 phase 为共享前置，二者不构成顺序循环。

退出条件：schema 结构检查通过；测试可证明 File/Directory `CLIENT_END` 可被 mlibc binding 使用，而测试不依赖 kernel VFS 实现。

### Phase 1：消除 kernel file 对 exec/mmap 的依赖

实现用户可创建 MObj、MObj executable loader、MObj-backed `MAP_PRIVATE`；将 process exec/spawn、ELF loader、VM map 的 `fs::vfs::file *` 依赖改为 MObj。

退出条件：现有 kernel VFS 下仍能启动 init，且从 MObj 启动一个 ELF、读取多个 LOAD segment、私有映射、超限/无 rights 的负向路径均被 C++ tests 覆盖。

### Phase 2：vfsd 与 boot module

新增 `naos/src/usr/vfsd/`、用户态 TAR loader/RAM backend、Directory/File server、materialize，实现独立 Multiboot module 和 early bootstrap。vfsd 完成 listener 注册后从 archive 启动 init。独立 `tarfsd` 不属于此阶段退出条件。vfsd 及后续全部 userland 服务（ramdiskd、filesystem worker）以 **Rust** 实现，消费 NaoIDL 生成的 Rust client/server binding（语言分配见 [VFS ADR](VFS_BLOCK_DEVICE_ADR.md) §10 r5）。

退出条件：QEMU cold boot 由 vfsd 启动 init；archive 畸形、vfsd listener 失败、init 不存在、materialize 超限均产生受控失败。

### Phase 3：mlibc 切换与兼容验证

移除 mlibc 对 File/Directory 必为 kernel view 的假设；root/cwd 只保存在 runtime；将 spawn/exec 和 private file mmap 切换到 sized MemoryObject + File pager。

退出条件：BusyBox 文件基本操作、relative/absolute/chroot path、symlink loop、open flags、fd offset sharing、fork/spawn bootstrap 在 QEMU 上通过；revision 2 的 `stat_node`/`sync`/`rename_at`/`link_at` 在 mlibc 与 Rust std 两侧同时可用，`lstat`、`fsync(dirfd)` 与跨目录 `renameat` 不再依赖被删除的模拟路径。

### Phase 4：删除内核 VFS

删除 `naos/src/kernel/fs/{vfs,ramfs,rootfs,pipefs}` 及对应私有 headers，删除 `publish_file_call`/`publish_directory_call` adapter、`native_directory`、`global_root`、kernel TAR loader、kernel VFS pseudo device 初始化和 VFS file-backed VM path。更新架构文档与 build graph。

退出条件：kernel 不含这些源和头文件依赖；默认 ISO boot、日志、tty service 启动和 BusyBox smoke test 都通过；无 hidden fallback。

### Phase 5：后续独立项目（不阻塞 MVP）

定义通用 Block/Device、Namespace/Mount 和 Pager 协议，增加 DevFS、可写持久 filesystem、generation pin、`MAP_SHARED`/writeback、service restart policy 与多用户凭据。每一项另立 PRD 和 ABI review。VFS namespace/mount 与 `BlockDevice` 的第一份对象契约见[用户态 VFS 与 BlockDevice 对象 ADR](VFS_BLOCK_DEVICE_ADR.md)；通用 pager、DevFS、格式实现和多用户策略仍需各自的 PRD。

### 6.1 跨平台 service transport 交付（横切，不改变 Phase 0–5 编号）

该交付轨道可在 Linux 上先行，不要求 QEMU，也不复制 daemon 业务实现：

当前实现状态：

| 轨道 | 当前状态 |
| --- | --- |
| L0 codec/transport 分层 | 已有 platform-neutral `RpcTransport`、纯 codec/generic client 和无默认 `naos-idl` codec 构建；生成式纯 Rust server/runtime skeleton 尚未完成。 |
| L1 Linux std + Tokio | 已实现固定 service-root、层级 URI/prefix discovery、长度前缀 UDS、独立 `ramdiskd`/`exfatd`/`vfsd` 进程；queue transport、完整 cancellation/backpressure 矩阵尚未完成。 |
| L2 Linux bulk/data plane | 已实现 BlockDevice 的 memfd + `SCM_RIGHTS` bulk 注册、descriptor 校验和 4 KiB/64 KiB/更大 region smoke；File 的 bulk 形式和跨重启持久化尚未完成。 |
| N1 NaOS std + Tokio adapter | 已将 native `ramdiskd`/`vfsd`/`exfatd` 编译为 Rust fork 的 `std` binary；NaOS Mio selector/waker、Tokio `naos` reactor feature、servicekit runtime facade 和 vfsd/exfatd 异步 wait loop 已接入。完整生成式 `NaosChannelTransport`、所有启动阶段同步 IDL 调用的异步化、cancellation/backpressure 矩阵仍未完成。 |

因此，下面的横切退出条件是目标验收清单，不是当前实现已全部满足的声明。

1. **L0：codec/transport 分层。** 将 NaoIDL 的纯 wire codec、method/revision/error manifest 与 `RpcTransport` trait 分离；生成的 client/server 不再绑定 `naos_sys` 或 Unix fd。`ramdiskd`、`vfsd`、`exfatd` 的 lease、namespace、FAT 和错误状态机保持平台无关。
2. **L1：Linux std + Tokio。** 同一 daemon crate 的 Linux bootstrap 选择 `LinuxUdsTransport`（优先 `SOCK_SEQPACKET`，或带长度前缀的 UDS stream）；可选 `LinuxQueueTransport` 使用 POSIX mqueue 或 eventfd + shared-memory ring。三个服务仍是独立 Linux 进程，不引入 TCP/UDP/HTTP/DNS。
3. **L2：Linux bulk/data plane。** UDS/queue 只传控制 descriptor；通过一次 `SCM_RIGHTS` 注册 `memfd`/shared ring，后续大段 read/write 只携带 `region_id`、`offset`、`length`、`direction`。memory backend 先完成，再增加 file-backed image 与重启持久化；同步 FAT 操作进入 Tokio blocking pool。
4. **N1：NaOS std + Tokio adapter。** 复用 L0 的 codec 和 service runner，以 NaOS 专用 Mio selector/waker 和 Tokio `WaitSet` 对接 Kernel Channel、NaoIDL invocation、epoll readiness、MemoryObject 和 capability disposition。NaOS 入口只保留 bootstrap/runtime 的 target-specific 代码；完整生成式 `NaosChannelTransport` 和剩余同步调用的异步化继续作为 N1 子项，不混入 transport 业务层。

横切轨道的 Linux 退出条件是：`cargo test --workspace`、三个独立进程的 UDS/queue smoke、bulk region 的 4 KiB/64 KiB/更大尺寸测试、lease/backpressure/cancellation、FAT 数据面和 vfsd `/data` 路由全部通过。当前已通过 workspace、UDS 三进程 smoke、BlockDevice bulk 和 `/data` 基础数据面；queue、完整 lease/backpressure/cancellation、File bulk、服务重启/持久化仍未完成。该退出条件不宣称 Linux opaque resource id 具备 NaOS kernel capability 的安全强度。

## 7. 错误、恢复和安全要求

- 服务端 protocol/domain error 使用现有负 errno；transport failure 保留 `NOT_DELIVERED` 与 `OUTCOME_UNKNOWN`，mlibc 映射为确定的 POSIX error。写入、create、rename 等非幂等操作不得对 `OUTCOME_UNKNOWN` 自动重试。
- vfsd crash 后已发出的 client endpoint 必须 `PEER_CLOSED`；新的服务实例只可通过显式 ServiceDirectory connect 获得，旧 fd 不得静默改绑。
- boot archive service 注册为一次性资源，只有 vfsd 能在启动阶段消费；init 和一般进程不会获得该 MemoryObject。vfsd 获得的 ServiceDirectory 权限仅允许 `naos://service/fs/vfs/0` 注册，或由独立 service manager 进一步缩小。
- 所有 archive offset/length、path length、symlink depth、directory cursor、channel bytes/resources 和 MObj size 必须做溢出及硬上限校验。
- kernel 不信任 vfsd 的 `Stat`、file type 或 executable 名称；它只验证 MemoryObject capability、rights、范围和 ELF bytes。ELF 无效只终止本次 exec/spawn，不破坏调用者现有地址空间。

## 8. 验证矩阵

| 层级 | 必测场景 |
| --- | --- |
| NaoIDL / client-server | File/Directory request/response、resource metadata、错误/peer close、inline 64 KiB bound、bulk descriptor/shared-region ownership、畸形 payload |
| vfsd | TAR 边界/overflow、普通文件/目录/link/symlink、RAM backend 语义、open offset、rename/unlink、materialize immutable snapshot |
| kernel | MObj rights、exec 的 ELF segment load、private mmap、bootstrap resource transaction、vfsd crash 后 capability/waiter cleanup |
| mlibc | `openat`、`readv/writev`、`getdents`、`chdir/chroot/getcwd`、`dup` offset sharing、spawn/exec、错误映射；revision 2 后加 `lstat/fstatat`、`fsync(dirfd)`、双 dirfd `renameat/linkat`（含 EXDEV 负向） |
| Rust std | 与 mlibc 行等价的同一操作集：`fs::metadata/symlink_metadata/rename/hard_link/symlink/read_dir`、`File::sync_all`、`chdir/current_dir`；错误 `ErrorKind` 映射与 manifest 一致 |
| Linux service transport | 三个独立 daemon 进程、UDS/queue round-trip、memory/file block backend、bulk 4 KiB/64 KiB/大于 64 KiB、backpressure/cancellation、服务重启；不依赖 QEMU |
| QEMU | cold boot、BusyBox smoke、文件服务故障、超限 executable、serial log 中无 VFS fallback |

遵从仓库测试规则：行为用 C++ tests、真实编译和 QEMU 验证；不添加用 Python 扫描/解析 C++ 源码来“证明”迁移完成的测试。

## 9. 实施风险与决策记录

| 风险 | 决策 / 缓解 |
| --- | --- |
| 启动鸡生蛋 | `vfsd` 独立 boot module；archive 作为 `rootdir` ServiceDirectory 资源发布，不从 kernel VFS 打开 vfsd。 |
| exec 重新依赖 VFS | 先落地 MObj executable，再删除 VFS。 |
| page fault deadlock | File pager 只由 kernel worker 发起 IPC；fault handler 不同步调用用户态服务。通用 pager 仍需独立 ABI review。 |
| 16 MiB MObj 上限 | 在 build/boot gate 中检查 archive/ELF；超限必须显式失败或先实现专用 immutable boot backing。 |
| 在 kernel 留下隐式 root/cwd | Phase 4 删除 `native_directory` 与 process 目录字段；bootstrap 只传递标准 namespace/stdio，其他内核资源通过 ServiceDirectory 发现。 |
| File/Directory 语义漂移 | 将 endpoint contract tests 同时跑在 temporary kernel adapter 与 vfsd server 上，直到 adapter 删除。 |
| TAR 被误当成长期开机依赖 | MVP 只把 TAR 视为当前镜像格式的用户态导入器；`tarfsd` 是可选拆分，不阻塞后续 FAT/ext2 等 backend。 |
| `/dev` path 仍在 mlibc 特判 | 明确标为过渡债务，后续由 DevFS 统一；本期不把它伪装成 kernel VFS。 |
| mlibc 仍调用 `SET_CURRENT`/`SET_ROOT` 并对 Directory 使用 `_na_handle_duplicate` | Phase 3 清偿：`clone_binding` 冻结后删除 kernel `process_t` 目录改写路径，runtime 只保存引用计数 binding。 |
| mlibc 路径 stat 用 open+stat 模拟且忽略 `AT_SYMLINK_NOFOLLOW`；`fsync` 固定发 FILE_SYNC 使目录 fd 错配失败；`renameat/linkat` 对不同 handle 直接 EXDEV | 三者均为登记债务，由 revision 2 的 `stat_node`/`sync`/`rename_at`/`link_at` 清偿（§5.3 第 6 条）；清偿前不得宣称对应 POSIX 特性受支持。 |
| Rust std / platform transport 分叉 | Linux 先用完整 `std + Tokio + RpcTransport`（UDS 或 Linux queue）完成无 QEMU 的独立进程验证；NaOS 复用同一 service/codec，以 `naos_std + naos_runtime + Kernel Channel` adapter 接入。Linux 的 std PAL、opaque resource id 和 `MemoryObject` 私有 fd backing 仅是本机 contract 模拟，不得误称为 NaOS capability 安全边界；大段数据必须走共享 data-plane，不能退化为 inline 全量复制。 |

## 10. 首个实现切片

建议第一张实现 issue 只做 Phase 1 的最小纵切：新增 MObj 创建与 executable frame 支持，将一个由 kernel test 创建并填充的 ELF MObj 用于 process spawn；暂不接入 vfsd 或改动 rootfs。它把最大架构风险（loader/VM 仍绑 VFS）先拆掉，并为随后可回滚的 vfsd 接入提供稳定边界。
