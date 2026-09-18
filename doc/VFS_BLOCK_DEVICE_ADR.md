# ADR：用户态 VFS 与 BlockDevice 对象

- 状态：Accepted（r7）
- 日期：2026-08-20（r2：2026-08-23；r3：2026-08-23；r4：2026-08-23；r5：2026-08-23；r6/r7：2026-08-24）
- 修订记录：r2 将 `NamespaceContext` 拆分为每可见根的 `NamespaceBinding` 与每 mount 的 `MountControl`，新增 `MutationTicket` 变更事务与全局 walk 预算；明确 v1 禁止 mount stacking、chroot 定位为传统路径视图而非安全边界、FAT32/exFAT 的 POSIX 能力边界见附录 A。r3（评审修订）：binding 的向上/截断路由改按 `mount_stack` 判定并定义各 binding 取值（新增 `MountControl.lookup_target`）；`rename_at`/`link_at` 第二 dirfd 改为 MOVE `clone_binding` 副本；新增只读 lease、`BlockInfo.medium_id`、flush 故障终结与错误判别规则、wire 枚举；ticket 明确为预留租约模型并补齐失败路径清理；Phase 0 generator 清单补 scope/right 未知名硬错、`@concurrent` 支持与 UUID/scope 碰撞检测。r4（Rust 可执行性评审）：新增“语言分配与交付边界”（v1 server 先行 C++，Rust 冻结 client + std 数据面）；Phase 0 增加 MObj 创建 syscall 前置、双侧 typed MemoryObject 封装、跨语言进程级互操作 test、generator 改造覆盖 Rust 后端；补 worker bootstrap 转移预算、私有对象转移机制与错误映射收敛要求。r4 复查补漏：`get_root`/`get_mount_info` 补 `@concurrent`；COMMITTING 期间 peer close 的终态定义为 `ABORTED`；`renameat` 与 `linkat` 的 backend 限定拆分；只读 lease 补验证背书，right 剥离的拒绝语义固定为 admission 层 `EACCES`（`EROFS` 归 mount 层）；unmount 非零 flags 定为 `EINVAL`；WalkContext 移除无 wire 载体的 hop 措辞；bootstrap 转移预算改述为 channel 消息资源上限。
r5（维护者裁定）：语言分配反转——`vfsd`、`ramdiskd` 与 filesystem worker 的 server 实现改以 Rust 交付；NaoIDL generator 的 Rust server/runtime/disposition-validator 模板从"后续 generator 工作"提升为本 ADR Phase 0 的前置交付项，Rust client + std 数据面承诺不变。
r6（评审修正）：`visible_root_node` 语义收窄为 chroot 包容边界——`enter_child_mount` 不再重置可见根（原 r4 文本使 mount 内绝对路径锚定子根、getcwd 显示 `/`，与"进入 mount 不等价于 chroot"矛盾）；绝对路径由 topology 下行穿越，跨 mount `path`/`getcwd` 由 parent_anchor 链在 vfsd 侧重建为正式 v1 能力；详见 §6.3/§6.4。
r7（评审修正，四项）：①挂载流程重排——初始根 NamespaceBinding 不再预发（NodeKey 为 worker 局部标识），commit 携带 `{root NodeKey, generation}`，根 binding 由 vfsd 在 commit 后经 bind_node 播种；②冻结重入规则：单线程同步 worker 合法，vfsd 禁止对未决 ticket worker 发起同步 MountControl RPC，删除 commit 前二次 lookup_target；③MutationTicket 共享 COMMITTING 态，消除本地提交期间超时释放预留的竞态；④unmount 显式状态机 ACTIVE→DRAINING→SYNCING→DETACHED→SHUTDOWN，明确 anchor 不计入 busy 引用并在 DRAINING 即关闭、child 先于 parent 卸载。
- 前置条件（按阶段分解；r5 修订：原文“Phase 4 已完成后才能开始”与本 ADR Phase 0 被 USERSPACE_FILESYSTEM_ADR Phase 0/2 消费的事实构成循环依赖，故拆分为；r6 同步修正 §6.2 初始根 binding 的可见根取值）：
  本 ADR **Phase 0**（NaoIDL generator 扩展、Rust server/runtime/validator 模板、schema 冻结、跨语言互操作 test）是与 [USERSPACE_FILESYSTEM_ADR](USERSPACE_FILESYSTEM_ADR.md) **共享的前置交付**，须在其 Phase 0–2 之前或同期落地；
  **Phase 1**（用户态 BlockDevice 纵向切片）要求文件系统迁移 Phase 1（MObj 数据面）已完成；
  **Phase 2**（vfsd route 与 worker 挂载事务）要求其 Phase 2 的 vfsd File/Directory server 已在运行；
  **Phase 3** 的“default boot 不依赖 optional disk、无 kernel/fs/* 回归”验收要求文件系统迁移 Phase 4 已完成。
- 相关决策：[Capability Handle 与异步 Invocation IPC](OBJECT_CALL_ADR.md) §8、§18、§19、§21.4。

## 1. 摘要

本 ADR 定义文件系统迁移后的下一层边界：

- `BlockDevice` 是供受信任存储服务使用的 typed **KernelView**，也是一个独占的逻辑块设备（LBD）区间租约。它只表示有界、异步、按逻辑块寻址的块 I/O；不表示 `/dev` 路径、物理盘、分区表或文件系统。
- `Vfs` 是由 **`vfsd` 进程唯一承载**的用户态 typed service。它拥有一个挂载 namespace、跨挂载路径解析和挂载生命周期；普通进程仍只使用 `Directory`/`File` endpoint。
- VFS 为每次挂载建立私有 namespace/lifecycle 控制连接；`fat32d`、`exfatd` 等 backend worker 持有独占 `BlockDevice` LBD lease，并直接服务其目录树中的 File/Directory endpoint。VFS 不接受设备名、`File`、裸物理地址或 ioctl request。

这使块驱动、格式实现（例如 FAT 或 ext2）和 namespace 策略可以独立演进，同时不把旧的 `global_root`、`mount_list`、`file_system::load()` 或 `dev::device` 数字 ID 重新引入 kernel ABI。

## 2. 背景与问题

已完成的文件系统 ADR 将 namespace、root/cwd、路径解析和文件数据移到 `vfsd`，并明确把 block device、mount 和持久写回留给独立阶段。旧 kernel VFS 的 `mount()` 以全局 dentry tree、`file_system *`、设备字符串和裸 `super_block *` 连接这些职责；它既不能表达 capability 授权，也无法隔离用户态格式解析。

当前 `MemoryObject` 适合受限的数据平面，但其实现还是内存 byte vector，尚无 DMA 固定、块设备 object、格式服务或 VFS backend contract。`Directory.open()` 的通用资源返回值也不足以作为跨服务、无竞态 path-walk 的 backend ABI。不能仅把 `mount(2)` 包成一次 RPC 而保留这些隐式关系。

## 3. 目标与非目标

### 3.1 目标

1. 冻结 `BlockDevice` 的 capability scope、rights、缓冲区、完成、持久性和错误语义。
2. 冻结 `vfsd` 承载的 `Vfs` 挂载 namespace 控制面；所有普通文件操作继续使用 `Directory` 和 `File`。
3. 使一个文件系统实例独占使用一个 `BlockDevice` LBD 区间，并作为一个可卸载的 source 交给 VFS。
4. 使受信任的 volume manager 能从一个物理介质派生多个互不重叠的 LBD；整盘与任一子区间不得同时获得数据面权限。
5. 使 kernel block I/O handler 在 admission 后异步完成，不在 syscall、IRQ 或持锁路径中等待设备。
6. 让 QEMU 的一个真实块设备和一个用户态 filesystem worker 能完成格式化后的挂载、写入、flush、卸载、重启后读取验证。

### 3.2 非目标

- 设备探测、PCI/总线枚举、driver 自动绑定、DevFS、udev 风格枚举、稳定的 `/dev/*` 名称、热插拔策略或任意应用直接获取 block capability；
- LVM、loop、RAID、加密、网络块设备或 block cache；分区表解析属于 `ramdiskd` 的实现策略，不属于 LBD 协议 ABI；
- page cache、user pager、`MAP_SHARED`、由缺页触发的 writeback，或 remote MemoryObject map；
- mount propagation、bind/overlay/move mount、lazy/forced unmount、mount namespace clone 和多用户凭据；
- 以泛型 `File.device_control` 或 POSIX `ioctl` 作为本对象的 ABI；
- 将 FAT/ext2 等任一具体格式的实现细节、repair policy 或 on-disk upgrade 规则固定在本 ADR。

首个实现只需一个用户态 `ramdiskd` block manager、一个可由 `vfsd` 按 mount instance 启动的 filesystem worker，以及由 `vfsd` 持有的单一系统 VFS namespace。worker 可以暂时内嵌在 `vfsd`，但其外部化不得改变 application-facing contract；独立进程必须经过相同的 capability 流程接口（bootstrap MOVE 与 NamespaceBinding/MountControl/MutationTicket 语义不变）。格式选择不能改写本文的 object contract。

当前 NaOS boot slice 已将 FAT worker 外化为独立的 `rootfs` boot module（当前实现为 `exfatd`）。kernel 将不可变 worker ELF 发布为 `naos://service/rootfs/0`，由 `ramdiskd` 通过 ServiceDirectory 查询；`ramdiskd` 自己创建 24 MiB 内存盘，维护 lease/queue/flush 状态并把用户态 factory 发布到 ServiceDirectory，随后启动 rootfs。rootfs 通过 block URI 解析 factory、自行 acquire LBD，再通过 VFS admin URI 完成 `prepare_mount`，因此 early-service bootstrap 只含 ServiceDirectory 与 stdio，不搬运任何 block 或 mount capability。worker 挂载已有 FAT 卷（空白介质才执行首次格式化）后提交 ticket，vfsd 才发布 mount record；固定的 24 MiB 模拟介质低于 fatfs 的 FAT32 cluster threshold，因此启动日志会明确报告 library-selected FAT 兼容模式。NamespaceBinding 初始端按 r7 不预发。MountControl dispatcher、bind_node/root-anchor 播种，以及 worker 直达的 Directory/File 核心数据面已经接通；worker 侧 `rename_at` 通过重入 path 查询支持同 worker 的跨目录 rename，`link_at` 按 FAT 能力矩阵返回 `EOPNOTSUPP`。物理块设备、完整 inode 持有语义和更大 FAT32 几何仍不在此模拟介质切片内。

Linux std 是同一套协议的独立进程模拟轨道，不冒充 NaOS capability 安全边界：`ramdiskd` 在固定 service-root 下发布 `naos://service/block/ramdiskd/0`，`exfatd` 通过 `RpcTransport`/UDS 获取 opaque block lease，`vfsd` 目前提供固定 `/data` mount 的 Directory/File 代理。Linux host 轨道尚未实现 NaOS 的 `NamespaceBinding`、`MountControl`/`MountTicket` 事务或跨重启持久化；这些仍由 NaOS boot slice 和后续完整 mount phase 验收。主机 IDL loopback 的 `application_dataplane` 测试与 Linux 三进程 UDS smoke 是两组不同测试：前者覆盖 worker 的 `rename_at`/`O_EXCL`/active-file `EBUSY`，后者覆盖真实进程边界的 format/read/write/fsync/truncate/rename 及 service discovery。

## 4. 架构与信任边界

```text
                         ┌────────────┐
                         │    vfsd    │
                         └─────┬──────┘
                               │ Directory/File
                         ┌─────▼──────┐
                         │ fatd/exfatd│
                         └─────┬──────┘
                               │ BlockDevice protocol
                 ┌─────────────┼─────────────┐
                 │             │             │
           ┌─────▼─────┐ ┌─────▼────┐ ┌──────▼─────┐
           │ ramdiskd  │ │  nvmed   │ │ virtioblkd │
           └───────────┘ └──────────┘ └────────────┘
```

`vfsd` 只负责 namespace、mount 和 Directory/File 路由；`fatd`/`exfatd`
负责具体文件系统语义，并通过 `BlockDevice` 协议使用底层 backend。`ramdiskd`
只是当前默认的内存 backend，未来 `nvmed`、`virtioblkd` 等服务注册同类
BlockDevice endpoint 后，不需要修改上层文件系统协议。

`BlockDeviceFactory` 是 `ramdiskd`/未来 `nvmed` 等用户态 block manager 提供的 IDL service，不是 kernel view。manager 持有 backing store 或 driver capability，按自己的策略选择 LBA 区间并创建不重叠的 `BlockDevice` endpoint；kernel 不创建 factory、lease table 或 BlockDevice endpoint。本期 NaOS kernel 不探测设备，也不创建 ramdisk backing store；它只负责普通 channel、MemoryObject 和 capability syscall。`ramdiskd` 通过 ServiceDirectory 发布持久 listener，mount worker 通过 URI `connect` 获取一个新的 factory client；listener 继续保留在目录中，后续 worker 可再次连接，单次连接对应的 factory server endpoint 由 manager 持有到 peer close。本切片的 exfatd 已按此发现流程落地。`vfsd` 不接触数据面。

LBD 的 authority 链在最终形态仍是单一的：block manager 负责 acquire、检查重叠并持有 lease，不存在 kernel factory 再向 manager 转交的第二授权链。当前 `exfatd` 通过 ServiceDirectory 解析用户态 factory 后直接 acquire LBD；MountControl 与 MountTicket 则由同一次 VFS admin `prepare_mount` 返回，NamespaceBinding 仍按 r7 在 commit 后由 vfsd 创建。

`vfsd` 是 Vfs server 的唯一实现和 namespace TCB，不另设 kernel VFS object、可链接的 VFS library 或第二个 namespace owner。它拥有 root route、mount topology、每可见根的 `NamespaceBinding`、每 mount 的 `MountControl` 与变更预留表；命中 mount 后可以返回 worker 签发的 direct `Directory`/`File` endpoint，但这些 endpoint 必须绑定 `vfsd` 签发的 NamespaceBinding，不能由 application 按地址自行连接 provider。这是 `..` 截断、chroot 后的路径解析、cross-mount rename、mountpoint 保护、endpoint accounting 和 safe unmount 的前提。现有 `naos://service/fs/vfs/0` listener 继续由 `vfsd` 提供普通目录树入口；`Vfs` admin endpoint 通过受信任的 ServiceDirectory URI 暴露给 mount manager，URI 本身只是 locator，返回的 Vfs capability 才是 authority。

mount manager 是策略而不是 kernel 子系统。当前切片由独立 worker 通过 ServiceDirectory 解析 block factory 与受信任的 `vfsd` admin endpoint，自己 acquire LBD、准备挂载并持有 `MountControl` server end 与 ticket；locator 名称不属于任何一个协议 ABI。初始 `NamespaceBinding` 不随 bootstrap 转移，待 worker commit 后由 vfsd 播种。

## 5. 对象模型与权限

| 对象 | Binding / owner | 权限 | 明确不代表 |
| --- | --- | --- | --- |
| `BlockDeviceFactory` | 用户态 `ClientEnd`；block manager 提供并持有 backing/driver | 查询介质并按用户态策略派生不重叠 LBD lease | kernel view、公开 `/dev` 枚举、设备探测或文件系统 |
| `BlockDevice` | 用户态 `ClientEnd`；block manager 为 worker 创建 | 一个 LBA 区间的独占 read/write/flush/discard（只读 lease 削减 write/flush/discard right，§6.1） | kernel object、路径名、物理盘、字符设备、文件系统、分区表或全局设备 ID |
| `MemoryObject` buffer | kernel 数据平面对象；复制给一个请求 | 恰由一个已接纳请求使用的字节区间 | 可写共享缓存或透明 file map |
| `NamespaceBinding` | `ClientEnd`；worker 持有（每可见根一份）、`vfsd` 服务 | 承载 `{namespace_instance, visible_root_node, current_node, mount_stack, rights}`：跨 mount 路由、根 `..`、absolute symlink、全局 walk 预算、派生 chroot binding 与变更预留入口 | 普通文件 read/write 数据面 |
| `MutationTicket` | `ClientEnd`；worker 持有、`vfsd` 服务 | 已预留元数据变更的预留租约：`commit`/`abort` 归还预留，`status` 幂等对账；状态机见 §6.2 state 表 | 查询、读取、回滚已提交的本地变更或未预留的写操作 |
| `MountControl` | `ClientEnd`；`vfsd` 持有、worker 服务 | `bind_root`、`bind_node`、`lookup_target`、sync、prepare_unmount 与 shutdown | application-facing 文件接口 |
| `Vfs` | `ClientEnd<Vfs>`；`vfsd` 是唯一 server，mount manager 持有 admin view | namespace、挂载图、跨挂载规则与 mount 生命周期 | 磁盘 driver、分区表策略或普通文件数据面 |
| `Directory` / `File` | `ClientEnd`；application 持有 | 经 VFS 检查的 node/open-description view | 挂载或访问原始设备的权限 |

持有 endpoint 即授权。manager 在 transfer 前必须削减协议语义与服务状态；不存在通过旧设备号进行的环境式查找。`BlockDevice` lease 没有 `DUPLICATE` right，只带 `TRANSFER`，因此只能 MOVE 给一个新 owner；raw backing capability 只留在 manager 内，绝不放入普通进程 bootstrap 或 POSIX fd table。

## 6. 稳定协议预留

本 ADR 预留下列 UUID/scope 对。Phase 0 必须在一次变更中将 public protocol 的 schema 加入 `idl/system/`，将私有 protocol 的 schema 加入仅供 `vfsd`/worker 使用的 internal IDL 目录，并将全部映射加入 `idl/naoidl.py`、生成 C++/Rust descriptor table 和 compatibility manifest。私有 protocol 的 header 只随 worker toolchain 分发，不构成 application SDK 承诺。在此之前，下列 NaoIDL 是规范性设计块，不是手写的替代 ABI。

| 协议 | UUID | scope | binding |
| --- | --- | --- | --- |
| `Vfs` | `2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1021` | 16 | `ClientEnd` |
| `BlockDevice` | `2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1022` | 17 | `KernelView` |
| `NamespaceBinding`（私有） | `2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1023` | 18 | `ClientEnd` |
| `MountTicket` | `2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1024` | 19 | `ClientEnd` |
| `BlockDeviceFactory` | `2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1025` | 20 | `KernelView` |
| `MountControl`（私有） | `2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1026` | 21 | `ClientEnd` |
| `MutationTicket`（私有） | `2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1027` | 22 | `ClientEnd` |

下列 protocol-right bit 同时预留。generator 必须拒绝未知的命名 right，不能静默生成不受限制的方法。

| NaoIDL 名称 | UAPI macro | bit |
| --- | --- | --- |
| `block_inspect` | `NA_BLOCK_DEVICE_RIGHT_INSPECT` | 13 |
| `block_read` | `NA_BLOCK_DEVICE_RIGHT_READ` | 14 |
| `block_write` | `NA_BLOCK_DEVICE_RIGHT_WRITE` | 15 |
| `block_flush` | `NA_BLOCK_DEVICE_RIGHT_FLUSH` | 16 |
| `block_discard` | `NA_BLOCK_DEVICE_RIGHT_DISCARD` | 17 |
| `vfs_lookup` | `NA_VFS_RIGHT_LOOKUP` | 18 |
| `vfs_mount` | `NA_VFS_RIGHT_MOUNT` | 19 |
| `vfs_inspect` | `NA_VFS_RIGHT_INSPECT` | 20 |
| `namespace_route` | `NA_NAMESPACE_BINDING_RIGHT_ROUTE` | 21 |
| `mount_control` | `NA_MOUNT_CONTROL_RIGHT_CONTROL` | 22 |
| `block_factory_inspect` | `NA_BLOCK_DEVICE_FACTORY_RIGHT_INSPECT` | 23 |
| `block_factory_acquire` | `NA_BLOCK_DEVICE_FACTORY_RIGHT_ACQUIRE` | 24 |
| `mount_ticket_control` | `NA_MOUNT_TICKET_RIGHT_CONTROL` | 25 |
| `mutation_ticket_control` | `NA_MUTATION_TICKET_RIGHT_CONTROL` | 26 |

Named right 按协议独立声明与校验：不同协议不得复用同一名称表达不同语义（例如 ticket 不借用 `vfs_mount`），generator 对未知名必须报错，对资源字段声明的协议权限不得静默丢弃。当前 `idl/naoidl.py` 的扁平 `METHOD_RIGHTS`/`disposition_rights` 表不满足该要求，Phase 0 必须改造。`BlockDevice.read/write` 的 MemoryObject buffer 字段还必须能声明方向性 memory-object 权限（read-only 与 write 区分）。generator 还必须先行修复三个会静默放行坏 contract 的缺口：资源字段声明的 protocol scope 名查无映射时必须编译失败，不得生成跳过 scope 校验的 validator；跨 schema 检查 UUID/scope 冲突；解析 `@concurrent` 并写入 descriptor 与 manifest——未标注方法的默认并发模型是 ADR §16.2 的 per-endpoint 串行执行。

### 6.1 `BlockDevice` 接口

`BlockDevice` 使用逻辑块地址（`lba`）域。它没有按字节寻址的 read 方法：每个操作都与 `logical_block_bytes` 对齐，成功的 I/O 必须完成全部请求块。对 filesystem worker 而言，`lba = 0` 永远表示自身 LBD 的首块；物理 `start_lba` 不出现在它的 `BlockInfo` 或任何数据面请求中。

```naidl
library naos.system;

struct BlockMediumInfo {
    u64 medium_id @id(1);          // opaque；仅在该物理介质生命周期内稳定
    u64 media_generation @id(2);   // 更换介质、调整大小或移除时变化
    u64 logical_block_bytes @id(3);
    u64 physical_block_bytes @id(4);
    u64 total_block_count @id(5);
    u64 max_transfer_blocks @id(6);
    u64 max_transfer_bytes @id(7);
    u64 max_in_flight @id(8);
    u64 features @id(9);
};

struct BlockInfo {
    u64 device_id @id(1);          // opaque；仅在此对象生命周期内稳定
    u64 media_generation @id(2);   // 更换介质、调整大小或移除时变化
    u64 logical_block_bytes @id(3);
    u64 physical_block_bytes @id(4);
    u64 block_count @id(5);
    u64 max_transfer_blocks @id(6);
    u64 max_transfer_bytes @id(7);
    u64 max_in_flight @id(8);
    u64 features @id(9);
    u64 medium_id @id(10);         // opaque；与 BlockMediumInfo.medium_id 同值域，判定共享排序域
};

protocol BlockDeviceFactory @uuid("2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1025") @revision(1) @features(0) @scope(20) @uapi_name("BLOCK_DEVICE_FACTORY") @scope_name("BLOCK_DEVICE_FACTORY") {
    method get_info @id(1) @rights(block_factory_inspect) -> {
        BlockMediumInfo value @id(1);
    };
    method acquire @id(2) @rights(block_factory_acquire) {
        u64 start_lba @id(1);
        u64 block_count @id(2);
        u64 flags @id(3);
    } -> {
        handle<block_device, transfer, move> device @id(1);
    };
};

protocol BlockDevice @uuid("2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1022") @revision(1) @features(0) @scope(17) @uapi_name("BLOCK_DEVICE") @scope_name("BLOCK_DEVICE") {
    method get_info @id(1) @rights(block_inspect) @concurrent -> {
        BlockInfo value @id(1);
    };
    method read @id(2) @rights(block_read) @concurrent {
        u64 lba @id(1);
        u64 block_count @id(2);
        u64 buffer_offset @id(3);
        handle<memory_object, memory_object_write + memory_object_map + transfer, duplicate> buffer @id(4);
        u64 flags @id(5);
    } ->;
    method write @id(3) @rights(block_write) @concurrent {
        u64 lba @id(1);
        u64 block_count @id(2);
        u64 buffer_offset @id(3);
        handle<memory_object, memory_object_read + memory_object_map + transfer, duplicate> buffer @id(4);
        u64 flags @id(5);
    } ->;
    method flush @id(4) @rights(block_flush) @concurrent ->;
    method discard @id(5) @rights(block_discard) @concurrent {
        u64 lba @id(1);
        u64 block_count @id(2);
    } ->;
};
```

`BlockDeviceFactory.acquire` 是物理介质唯一的租约入口。它验证 `start_lba + block_count` 不溢出且位于 `total_block_count` 内，并在同一 `media_generation` 下维护一个非重叠区间表：任何与活动 lease 相交的请求返回 `EBUSY`，包括整盘 `[0, total_block_count)` 与任一分区的组合。v1 `acquire.flags` 只接受零和 `READ_ONLY = 1 << 0`：置位时签发的 lease 不含 `block_write`/`block_flush`/`block_discard` right，写入类方法在 admission 层即被 rights 校验拒绝（status 经 mlibc 映射为 `EACCES`）；声明 `READ_ONLY` feature 的只读介质一律签发此类只读 lease。POSIX 语义中的 `EROFS` 由 mount 的 READ_ONLY 标志在文件层产生（§6.2），不由 lease 层产生。

返回的 `BlockDevice` 是一个带有隐式 `{ physical_medium, start_lba, block_count, media_generation }` 的 unique KernelView。它没有 `DUPLICATE` right，只能通过 MOVE 改变 owner；last close 后，factory 仅在该 lease 的 in-flight I/O 全部完成、相关 MemoryObject 引用释放后才回收区间。`ramdiskd` 可先取得整盘 lease 读取 GPT/MBR 或固定配置，再关闭该 lease 并为不重叠分区分别 `acquire`。当任一子 LBD 活跃时，`ramdiskd` 不能再取得整盘或重叠区间以修改分区表。

这一定义支持分区而不把分区表格式塞入 kernel：block manager 决定要请求哪些区间，并在自己的服务端以不可绕过的服务边界执行 lease。未来 LVM、loop 或远端卷可以在自己的可信 manager 中实现同样的 range-lease 规则，并继续向 filesystem worker 暴露 `BlockDevice`，无需改变其 I/O ABI。

`block_inspect` 只允许 `get_info`；`block_read`、`block_write`、`block_flush` 和 `block_discard` 分别授权同名方法。只读 view 不包含 write、flush 和 discard right。`get_info.features` 是至少包含 `READ_ONLY`、`FLUSH`、`FUA`、`DISCARD` 和 `VOLATILE_WRITE_CACHE` 的 bitset；必须忽略未知 bit。v1 的 `write.flags` 只接受 `FUA = 1 << 0`，且仅在声明该 feature 时有效；`read.flags` 必须为零。未来协议 revision 通过新增方法/字段而不是 ioctl，增加 topology、secure erase 或 write-zeroes。

| bitset | 稳定 v1 bit |
| --- | --- |
| `BlockInfo.features` | `READ_ONLY = 1 << 0`, `FLUSH = 1 << 1`, `FUA = 1 << 2`, `DISCARD = 1 << 3`, `VOLATILE_WRITE_CACHE = 1 << 4` |
| `BlockDevice.write.flags` | `FUA = 1 << 0` |
| `BlockDeviceFactory.acquire.flags` | `READ_ONLY = 1 << 0` |
| `Vfs.prepare_mount.flags` | `READ_ONLY = 1 << 0` |

`features` 与各 flags 域的 UAPI 宏名在 Phase 0 冻结为：`NA_BLOCK_DEVICE_FEATURE_READ_ONLY/FLUSH/FUA/DISCARD/VOLATILE_WRITE_CACHE`（bit0–4）、`NA_BLOCK_DEVICE_WRITE_FLAG_FUA`、`NA_BLOCK_DEVICE_FACTORY_ACQUIRE_FLAG_READ_ONLY`、`NA_VFS_PREPARE_MOUNT_FLAG_READ_ONLY`——同名语义位分属不同字段，不共用宏。`MountTicket.status` 的 state 值经 public 协议暴露，UAPI 化为 `NA_MOUNT_TICKET_STATE_PREPARED/COMMITTING/COMMITTED/ABORTED/EXPIRED`；`begin_mutation` 的 `operation` 值仅存于私有协议，留在 internal IDL 目录，不进 UAPI。

#### 请求、缓冲区和完成规则

- `block_count` 不能为零、不能大于 `max_transfer_blocks`，且 `lba + block_count` 不得溢出或超过 `BlockInfo` 中的 `block_count`。
- `block_count * logical_block_bytes` 不得溢出、不得超过 `max_transfer_bytes` 或所给的 MemoryObject 区间。`buffer_offset` 和字节长度必须按逻辑块对齐。
- 请求将 buffer capability 复制到 invocation。调用方保留原 handle；kernel 直到该请求得到唯一终态结果前始终保留 backing object 的强引用，不向用户态返回这份复制。
- `write` 在字节区间对 driver queue 可见前采样精确内容。实现必须固定不可变 DMA backing，或在 admission 时创建有界的 bounce copy。因此调用方在 submit 后修改 MemoryObject，不能改变已接纳写入。
- `read` 的目的区间在成功完成前内容未定义。成功时它含有完整请求区间；失败 read 没有部分成功响应，调用方必须视整个区间为无效。
- 禁止把用户字节区间复制进 control message。仅当 MemoryObject backing 不能 DMA map 时，driver 才能使用有界、计入 quota 的 kernel bounce buffer。
- `read`、`write`、`flush` 和 `discard` 都是异步 invocation 操作。admission 必须在任何设备副作用前分配全部 request、pin/bounce 和 queue accounting。队列满时返回 `WOULD_BLOCK` 或 `RESOURCE_EXHAUSTED`；kernel 不得 busy-wait，也不得在 IRQ-disabled 区间运行 driver。
- I/O 方法标记为 `@concurrent`，以允许一个 endpoint 同时有多个 in-flight request。kernel 在入队前分配单调递增的设备 admission 序号；即使 method handler 乱序完成，`flush` 也必须等到所有较小序号的 write 到达设备完成点。
- 已完成的 `write` 仅保证设备已接收，不保证持久化，除非其 `FUA` flag 被接受。`flush` 仅在该设备排序域中所有早于它被接纳的 write 都已持久化后完成；它不是 per-client cache flush。
- admission 序号空间与 flush 排序域按物理设备划分：`medium_id` 相同的全部 LBD lease 共享同一序号空间，一个 lease 的 `flush` 因此会等待同介质其它 lease 上较小序号 write 的完成点；这是共享易失缓存的物理后果，filesystem 之间由此产生有限的完成延迟耦合。
- 任一较小序号的 write 以 domain error 或 transport `OUTCOME_UNKNOWN` 终结后，`flush` 不再等待其持久化确认，而是携带该事实以 `EIO`（介质移除为 `ENODEV`）确定性失败完成；终结状态保留在该介质的序号账本中，后续 `flush` 同样立即失败。
- `discard` 是可选操作，仅在声明对应 feature 时有效。它释放指定区间，不保证后续 read 返回零；filesystem 不得将它用作元数据持久化原语。
- dispatch 后的请求取消、client close 或 wait timeout 都不取消设备 I/O。无法证明未执行时，通用 invocation 结果保持 `OUTCOME_UNKNOWN`；`write`、`flush` 和 `discard` 绝不自动重试。

错误判别规则固定如下：区间越界、非对齐、溢出和未定义的高位 flag 返回 `EINVAL`；位已定义但设备不具备对应 feature（未声明 `FLUSH` 时 `flush`、未声明 `DISCARD` 时 `discard`、未声明 `FUA` 时置位 FUA）返回 `EOPNOTSUPP`；设备 request 池（即设备 queue 槽位账本）耗尽返回 `WOULD_BLOCK`，kernel 全局 pinned/bounce page quota 耗尽返回 `RESOURCE_EXHAUSTED`——两者是 submission status 层承载 block 域语义的特例，不进入 `protocol_error`（`WOULD_BLOCK` 与既有 `EAGAIN` 同值异名，manifest 以 `WOULD_BLOCK` 为规范名）；介质移除返回 `ENODEV`，介质在位但命令/数据错误返回 `EIO`；`media_generation` 变化对 BlockDevice 方法表现为 `ENODEV`/`EIO`，filesystem/worker 层将其呈现为 `ESTALE`（§7），`ESTALE` 不属于 BlockDevice 域错误集；只读 lease 上的写入类方法在 admission 层被 rights 拒绝（映射 `EACCES`），见 §6.1。transport admission error 必须与 filesystem domain error 保持区分。

### 6.2 `vfsd` 挂载与 worker 启动接口

`Vfs` 是 `vfsd` 的 control-plane 协议。`Directory` 和 `File` 是全部通用 POSIX-facing 请求的唯一入口：它们可由 `vfsd` 或当前目录所在 worker 直接服务，但 application 不直接发现、连接或管理 worker。`Vfs` 不是 `openat()` 的另一种拼写。

```naidl
library naos.system;

struct MountInfo {
    u64 mount_id @id(1);       // 仅在此 Vfs instance 内 opaque
    u64 namespace_generation @id(2);
    u64 flags @id(3);          // 回显 commit 生效的 prepare_mount flags；v1 仅 READ_ONLY
    u64 backend_generation @id(4); // worker backend 实例版本计数：backend 重启/重建时递增；诊断用，不参与授权
    u64 device_id @id(5);      // commit 时分配的稳定伪 st_dev
};

protocol Vfs @uuid("2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1021") @revision(1) @features(0) @scope(16) @uapi_name("VFS") @scope_name("VFS") {
    method get_root @id(1) @rights(vfs_lookup) @concurrent -> {
        client_end<directory, transfer, move> root @id(1);
    };
    method prepare_mount @id(2) @rights(vfs_mount) @max_bytes(65536) {
        u64 target_size @id(1);
        bytes<4095> target @id(2) @inline @length_field("target_size");
        u64 flags @id(3);
    } -> {
        client_end<namespace_binding, transfer, move> namespace_binding @id(1);
        server_end<mount_control, transfer, move> control @id(2);
        client_end<mount_ticket, transfer, move> ticket @id(3);
    };
    method unmount @id(3) @rights(vfs_mount) @max_bytes(65536) @concurrent {
        u64 target_size @id(1);
        bytes<4095> target @id(2) @inline @length_field("target_size");
        u64 flags @id(3);
    } ->;
    method sync @id(4) @rights(vfs_mount) @concurrent {
        u64 mount_id @id(1);
    } ->;
    method get_mount_info @id(5) @rights(vfs_inspect) @concurrent {
        u64 mount_id @id(1);
    } -> {
        MountInfo value @id(1);
    };
};

```

`get_root`、`get_mount_info`、`unmount` 与 `sync` 均标记 `@concurrent`：慢速 worker lifecycle RPC（可能等待整盘 flush）不得阻塞同 endpoint 上的轻量查询；并发正确性由 per-mount-record 锁与 topology 锁约束——同一 mount 的控制操作仍串行，不同 mount 互不阻塞。`prepare_mount` 保持默认串行：topology 变更点是全局临界区，进行中预留之间的 `EBUSY` 由共享预留表在串行 dispatcher 下自然产生。

```naidl
protocol MountTicket @uuid("2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1024") @revision(1) @features(0) @scope(19) @uapi_name("MOUNT_TICKET") @scope_name("MOUNT_TICKET") {
    method commit @id(1) @rights(mount_ticket_control) {
        u64 root_node @id(1);          // worker 铸造的本地根 node_id（r7）
        u64 root_generation @id(2);
    } -> {
        MountInfo value @id(1);
    };
    method abort @id(2) @rights(mount_ticket_control) ->;
    method status @id(3) @rights(mount_ticket_control) @idempotent -> {
        u32 state @id(1);
    };
};
```

r7 注：`commit` 由空请求改为携带 worker 初始化格式后铸造的根节点标识；vfsd 据此在 commit 后通过 MountControl 播种根 NamespaceBinding（§6.2）。当前 exfatd 已消费该请求并在格式化后提交；其余 worker 类型沿用同一 wire contract。

`MountTicket.status` 与 `MutationTicket.status` 返回同一张规范性 state 表（wire 值冻结）：`PREPARED = 0`、`COMMITTING = 1`、`COMMITTED = 2`、`ABORTED = 3`、`EXPIRED = 4`。两类 ticket 都使用 `COMMITTING`：MountTicket 在 vfsd 受理 `commit` 时进入；MutationTicket 同样在 vfsd 受理其 `ticket.commit`（归还预留的调用）时进入——进入后过期计时立即停止且 `EXPIRED` 不可达（r7 修正：原文本令 MutationTicket 可在 worker 本地元数据提交期间因超时进入 EXPIRED 并释放预留，而本地副作用不可撤销，构成检查/提交竞态）。`NamespaceBinding.begin_mutation` 的 `operation` 取值同样冻结：`RENAME = 0`、`LINK = 1`、`SYMLINK = 2`、`CREATE = 3`、`MKDIR = 4`、`UNLINK = 5`、`RMDIR = 6`；`rename_at`/`link_at` 复用 `RENAME`/`LINK`。

`vfs_lookup` 只授予 `get_root`；`vfs_mount` 授予 mount/unmount/sync 操作；`vfs_inspect` 允许查询 metadata。这些 scope 和 right 不放入普通 application bootstrap。`MountTicket` 使用独立的 `mount_ticket_control` 权限，不复用 `vfs_mount`：worker 仅凭 ticket 本身即可提交结果，不会被解释为 Vfs admin 授权。`NamespaceBinding`、`MountControl` 和 `MutationTicket` 是私有 NaoIDL protocol；它们不进入 public SDK，不注册 ServiceDirectory locator，也不能被 application 从 URI 重新获取。

`target` 是 Vfs instance 根目录下的绝对、无 NUL 字节路径。`prepare_mount` 相对于 VFS 的 rename/create/remove 操作，以原子方式解析并预留精确目标 node；最后一个 component 必须是真实目录，若它是 symlink 则不能跟随。target 已被活动 mount 占用（含其它进行中的 prepare_mount 预留）时返回 `EBUSY`：**v1 禁止 stacking/over-mount**，每个 `{parent_mount, mountpoint}` 至多对应一个 child mount，mount record 因此保持单槽。根 mount 不能 unmount。v1 flags 只接受零和 `READ_ONLY`；完全未知的 flag 返回 `EINVAL`，位已定义但不支持的返回 `EOPNOTSUPP`，不得静默忽略。目标解析与预留机制：`vfsd` 先按拓扑把 target 拆解为 owning mount 与其内部相对路径，调用该 mount 的 `MountControl.lookup_target`（§6.4）在 worker 元数据锁内原子取得 `{parent_dir NodeKey, 末组件名, node NodeKey}`；`vfsd` 随即以 `{owner mount, parent_dir, 末组件名}` 为键写入与 mutation 共享的预留表并签发 ticket——预留先于任何发布。r7 修正：**commit 前的第二次 lookup_target 重验被删除**——预留表本身在 begin_mutation 处拦截一切触及 `{owner mount, parent_dir, 末组件名}` 的变更（否则 EBUSY），检查与提交之间替换或新建挂载点因此不可能；且该重验会在 worker 等待 commit 结果时构成 vfsd→worker 反向同步调用（死锁，见下）。

成功时，`prepare_mount` 返回两个 capability：`MountTicket` 与该 mount 的 `MountControl` server end（worker→`vfsd` 的初始根 NamespaceBinding **不再预发**——r7 修正：NodeKey 是 worker 局部标识，vfsd 在 worker 初始化格式之前无从知晓其根节点）。mount manager 将这两个 capability 与一份新取得的 LBD lease、格式 kind/options 一起通过 child bootstrap MOVE 给新 worker。worker 挂载或格式化磁盘、铸造本地根 `{node_id, generation}` 后，调用携带请求 `{root NodeKey @id(1), root generation @id(2)}` 的 `MountTicket.commit`。`vfsd` 在处理 commit 时**不向 worker 发起任何同步 MountControl 调用**：校验预留与 generation 后发布 mount record，回复 MountInfo；随后才经 `MountControl.bind_node(root_node, 0, binding)` 取得绑定该 binding 的根 Directory endpoint 作为 root_anchor 种子——此时 worker 已无未决 ticket，处于正常服务状态，不构成重入死锁。当前 exfatd 已完成 bootstrap/commit、bind_node/root-anchor 播种，以及 Directory/File 核心数据面；不在首期 FAT 能力矩阵内的操作明确返回 `EOPNOTSUPP`。

**重入规则冻结（r7）。** v1 明确允许单线程同步 serve loop 的 worker 实现。约束落在 vfsd 侧：对拥有未决（PREPARED/COMMITTING）ticket 的 worker，禁止发起任何同步 MountControl RPC；`lookup_target` 仅对已 COMMITTED 的 mount 发起。配合 §8 的"持拓扑锁不等待 worker RPC"，两条通道互不阻塞不需要 worker 具备并发 dispatcher；选择并发实现的 worker 同样合法。

`MountTicket` 具有显式状态机 `PREPARED → COMMITTING → COMMITTED | ABORTED`，外加终态 `EXPIRED`：仅 `PREPARED` 可进入 `COMMITTING`，且超时计时只在 `PREPARED` 期间运行——`EXPIRED` 仅从 `PREPARED` 可达。使 `PREPARED` 状态的 ticket 进入 `EXPIRED` 并释放全部 reservation accounting 的触发集为：预留超时、任一 peer close，或所属 mount 失效（unmount 完成、`media_generation` 变化、`MountControl` 连接断开）。进入 `COMMITTING` 后 ticket 不再转 `EXPIRED`：worker 在等待 commit 结果期间崩溃或断连，表现为该次 `commit` invocation 失败或 `OUTCOME_UNKNOWN`，`vfsd` 放弃发布并将 ticket 置为 `ABORTED`、释放预留（随后可用幂等 `status` 对账）；`vfsd` 自身消失由进程终止规则统一覆盖。`commit` 与 `abort` 竞争唯一终态转换，失败方返回 `ALREADY_CONSUMED`。`status` 是幂等查询，专用于 `OUTCOME_UNKNOWN` 后的对账——调用者不得盲目重发 `commit`，只能依据 `status` 判定已发布或已回滚，再决定重建 ticket 或清理。若 target identity/generation 已改变、worker 初始化失败、格式不受支持或介质损坏，则 commit 不发布任何 mount；worker 关闭 LBD lease，factory 在清理 in-flight I/O 后回收区间，mount manager 可按需要重新 acquire。返回的 `mount_id` 仅作诊断，并且只在该 Vfs endpoint 存活时有效。`device_id` 是 commit 时分配的稳定伪 st_dev：同一 mount instance 内所有节点返回的 `Stat.device` 都使用该值，与 worker 本地 inode 号一起构成跨 mount 唯一的 `{st_dev, st_ino}`（既有 `Stat.device_id` 是 st_rdev 字段，与本机制无关）；它同时是双 dirfd 原子操作（USERSPACE ADR §5.3 的 `rename_at`/`link_at`）判定“两端是否同一 filesystem instance”的权威依据——不一致即无副作用地返回 `EXDEV`。

普通 `unmount` 仅接受零 flags（非零返回 `EINVAL`）。r7 起每个 mount record 携带显式生命周期状态机 `ACTIVE → DRAINING → SYNCING → DETACHED → SHUTDOWN`（终态后记录销毁；任何一步失败使 mount 回到 `ACTIVE` 并保持可用，sync error 除外——见下）：

- **ACTIVE → DRAINING**：`vfsd` 原子完成两件事——(1) topology 标记该 mount 为 draining，此后跨 mount traversal 不再进入它（`enter_child_mount`/absolute 下行视作无 child mount）；(2) 向 worker 发出 `MountControl.prepare_unmount`。worker 在自身元数据锁内原子进入本地 DRAINING：拒绝一切新 open/create/mutation 类请求（`EBUSY`），已接纳的 read/write/list 等继续完成，in-flight 计数归零且无应用持有的直接 File/Directory endpoint、无活跃 NamespaceBinding、无未结 ticket 时返回成功，否则 `EBUSY` 并回退 ACTIVE。
- **引用计数口径**：应用与 worker 持有的 endpoint/binding/ticket 计入 busy；`root_anchor`/`parent_anchor` 是 vfsd 私有缓存，**不计入**（否则永远 EBUSY）。DRAINING 开始后 topology 立即停止向该 mount 路由，vfsd 不得再经 anchor 派生新的 endpoint；私有 anchor 保留到 worker shutdown 成功，以便失败回滚到 ACTIVE 时不丢失现有数据面，随后随 DETACHED 一起关闭。
- **DRAINING → SYNCING**：`MountControl.sync`（worker 最终执行 `BlockDevice.flush`；只读 lease 的 mount 无脏数据，sync 直接成功、不触发 flush）。sync error 使 mount 回到 ACTIVE 且保持挂载可用。
- **SYNCING → DETACHED**：原子移除 topology 记录、释放全部预留表项并令该 mount 未结 ticket 进入 EXPIRED；child mount 先于 parent 卸载（v1 森林拓扑下的后序遍历，parent 的 DRAINING 因 traversal 停止而自然等待 child 引用清空）。
- **DETACHED → SHUTDOWN**：`MountControl.shutdown` 通知 worker 关闭 LBD lease 并退出；worker 退出即 SHUTDOWN。

v1 禁止 stacking，因此不存在被覆盖 node 的恢复问题。`Vfs.sync` 只经 control 连接同步一个 mount instance，不表示全局 filesystem flush。

### 6.3 NamespaceBinding 与 `chroot`

`vfsd` 为每一个可见根边界签发一个私有的 `NamespaceBinding`。它是 Directory endpoint 携带的上下文，而不是数据面：

```text
{ namespace_instance, visible_root_node, current_node, mount_stack, rights }
```

其中 `visible_root_node` **只承担 chroot 包容边界语义**：它是 `..` 与绝对路径解析不得越过的边界（chroot 派生 binding 为目标目录，其余一切 binding——包括经 `enter_child_mount` 进入子 mount 的端点——保持签发链上游的可见根，通常为系统根）。它**不是**显示基准，也不因进入 mount 而改变。binding 随 endpoint 存活：`Directory.clone_binding` 与同进程运行时引用共享同一个 binding；`derive_chroot`（§6.4）创建子 binding；最后一个引用关闭时 binding 归还 per-mount accounting。application 永远看不到 binding 对象本身。`namespace_instance` 是本次 vfsd namespace 的 epoch 标识（vfsd 重启后变化，用于拒绝旧 binding 的跨实例使用）；`mount_stack` 对 worker 不透明，非空表示该 binding 经 namespace 进入过子 mount，仅在根 `..` 二分、绝对路径的 topology 下行与 `route_above` 合法性判定中使用。

worker 在本地目录内解析相对路径；以下情况经 binding 回到 `vfsd`：absolute path、absolute symlink、本地文件系统根上的 `..`、mountpoint 命中与变更预留。绝对路径解析以 `visible_root_node` 为起点、由 `vfsd` 按 topology 向下穿越 mount（沿途 mountpoint 命中即切换 worker），因此 `open("/etc")` 在任何非 chroot binding 上都解析到系统根的 `/etc`，与进程从哪个目录进入 mount 无关。在本地文件系统根执行 `..` 时按 binding 二分：`mount_stack` 非空则调用 `route_above` 上升（mount 进入不构成边界），`mount_stack` 为空且 `current_node == visible_root_node`（系统根或 chroot 派生）则原地截断。`Directory.path`/`getcwd` 由 `vfsd` 用 parent_anchor 链组合重建完整宿主路径：worker 先给出相对本 mount 根的本地路径，vfsd 沿挂载链向上拼接各挂载点的宿主前缀；chroot 派生 binding 在可见根处截断显示为 `/`。跨 mount 的 getcwd 重建因此是 v1 正式能力而非限制；唯一不精确场景是 chroot 边界之外的宿主前缀按设计不显示。

保留现有 `NA_DIRECTORY_OPEN_FLAG_CHROOT`，其 user-service 语义固定如下：从一个输入 Directory context 解析目标目录，worker 对目标本地节点调用 `derive_chroot`，得到 `{visible_root_node = target_directory, current_node = target_directory}` 的派生 binding，并在本地物化绑定它的 endpoint。

`chroot(path)` 的 mlibc 实现调用带该 flag 的 `Directory.open`，要求结果为目录，然后将 root 与 cwd 都替换为该**同一个**受限 Directory binding。NaOS 的该兼容语义有意同时重置 cwd，避免保留 chroot 外 cwd 后经 `..` 逃逸的历史 POSIX 陷阱。`Directory.set_root` 与 `Directory.set_current` 对 user-service endpoint 不得再改写 kernel `process_t`；它们在兼容期内返回 no-op/`ENOTSUP`，mlibc 只更新自己的 runtime binding。

**chroot 定位为传统路径视图，而不是安全边界。** 本设计不引入按进程可撤销的 binding membrane：依据 [ADR](OBJECT_CALL_ADR.md) §9.7，capability 一旦转移即不可递归撤销，endpoint 也不可改绑（§11.7）。因此 chroot 只约束**之后**经由新 binding 进行的路径解析；chroot 之前打开的目录 fd、既有的 File/Directory capability 与经 bootstrap 收到的 endpoint 仍是有效授权，可以继续用于访问新根之外——这与 Linux/Fuchsia 的实际保证一致。强 capability 隔离（epoch 化、可撤销的 binding membrane）若将来需要，另立 sandbox PRD，不改写本文契约。

一个进程内 root/cwd 指向同一目录时，mlibc 以引用计数的 `directory_binding` 共享同一个 native handle，不能为两个变量调用 `handle_duplicate`。跨进程 fork/spawn 由服务端签发新的 unique endpoint 完成：为此，`Directory` 的下一兼容 revision 增加：

```naidl
method clone_binding @id(14) -> {
    client_end<directory, transfer, move> directory @id(1);
};
```

它返回同一 `{visible_root_node, current_node, mount_stack, rights}` 的新 binding 引用与新 endpoint；由 mlibc 在构造 child bootstrap message 前调用。冻结规则：同进程 `dup` 是 runtime 引用计数共享，对 Directory endpoint 调用 `handle_duplicate` 视为编程错误；fork/spawn 的每个可继承目录 fd 都必须替换为 `clone_binding` 结果或既有独立 endpoint，任一步失败则整体回滚 fork。它不能共享公开 endpoint，也不能扩大目录权限。`chdir`、`fchdir` 和普通 `Directory.open` 返回的目录默认继承输入 endpoint 的 `visible_root_node`，只改变 `current_node`；只有 `NA_DIRECTORY_OPEN_FLAG_CHROOT` 能缩小这一边界。Directory revision 2 的完整方法清单（`clone_binding`/`stat_node`/`sync`/`rename_at`/`link_at` 及其语义）见 [USERSPACE_FILESYSTEM_ADR](USERSPACE_FILESYSTEM_ADR.md) §5.3；其中 `rename_at`/`link_at` 的第二个 dirfd 以 MOVE 方式传入：mlibc 先对原 dirfd 调用 `clone_binding` 生成临时副本、MOVE 副本进请求，服务端使用后即弃，调用者的原 handle 全程保留。MOVE 不触碰 ADR §11.2 对 endpoint 复制权的限制，dirfd 也无需携带 meta DUPLICATE right；每次调用多出的一次 clone RPC 是 v1 有意接受的成本。

必须在 `vfsd` 的 C++ protocol contract test 中覆盖：chroot 后的 absolute/relative lookup、根目录 `..`、absolute/relative symlink、`getcwd`、`fchdir`、fork/spawn 继承与 peer close。测试必须证明 chroot **之后**创建或派生的 binding 不能经路径解析越过 `visible_root_node`；同时将“chroot 前 open 的目录 fd 仍可用于 `openat` 访问界外”作为文档化行为测试（预期成功），防止实现误把它当作需要修复的违规。

### 6.4 直连 worker 与私有控制协议

私有 protocol 的最小 revision 如下。`NodeKey` 是 worker 局部且带 generation 的稳定标识，绝不是裸 inode 指针；worker 删除后复用 node ID 前必须改变 generation。

```naidl
library naos.internal;

struct NodeKey {
    u64 node_id @id(1);
    u64 generation @id(2);
};

struct WalkContext {
    u64 open_flags @id(1);          // O_NOFOLLOW 等 lookup 语义
    u32 remaining_symlinks @id(2);  // 全局 symlink 预算
    u32 reserved @id(3);            // 必须为零
};

protocol NamespaceBinding @uuid("2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1023") @revision(1) @features(0) @scope(18) @uapi_name("NAMESPACE_BINDING") @scope_name("NAMESPACE_BINDING") {
    method resolve_absolute @id(1) @rights(namespace_route) @max_bytes(65536) {
        WalkContext walk @id(1);
        u64 path_size @id(2);
        bytes<4095> path @id(3) @inline @length_field("path_size");
    } -> {
        handle<resource, transfer, move> object @id(1);
    };
    method route_above @id(2) @rights(namespace_route) -> {
        client_end<directory, transfer, move> directory @id(1);
    };
    method enter_child_mount @id(3) @rights(namespace_route) {
        NodeKey mountpoint @id(1);
    } -> {
        client_end<directory, transfer, move> directory @id(1);
    };
    method derive_chroot @id(4) @rights(namespace_route) {
        NodeKey subtree_root @id(1);
    } -> {
        client_end<namespace_binding, transfer, move> binding @id(1);
    };
    method begin_mutation @id(5) @rights(namespace_route) @max_bytes(65536) {
        u32 operation @id(1);
        NodeKey old_parent @id(2);
        NodeKey old_target @id(3);
        NodeKey new_parent @id(4);
        NodeKey new_target @id(5);
        u64 old_name_size @id(6);
        bytes<255> old_name @id(7) @inline @length_field("old_name_size");
        u64 new_name_size @id(8);
        bytes<255> new_name @id(9) @inline @length_field("new_name_size");
    } -> {
        client_end<mutation_ticket, transfer, move> ticket @id(1);
    };
};

protocol MutationTicket @uuid("2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1027") @revision(1) @features(0) @scope(22) @uapi_name("MUTATION_TICKET") @scope_name("MUTATION_TICKET") {
    method commit @id(1) @rights(mutation_ticket_control) ->;
    method abort @id(2) @rights(mutation_ticket_control) ->;
    method status @id(3) @rights(mutation_ticket_control) @idempotent -> {
        u32 state @id(1);
    };
};

protocol MountControl @uuid("2b9e3c2e-8c7d-4fb1-9e21-4c4b0e0a1026") @revision(1) @features(0) @scope(21) @uapi_name("MOUNT_CONTROL") @scope_name("MOUNT_CONTROL") {
    method bind_root @id(1) @rights(mount_control) -> {
        client_end<directory, transfer, move> directory @id(1);
    };
    method bind_node @id(2) @rights(mount_control) {
        NodeKey node @id(1);
        u64 flags @id(2);
        client_end<namespace_binding, transfer, move> binding @id(3);
    } -> {
        client_end<directory, transfer, move> directory @id(1);
    };
    method sync @id(3) @rights(mount_control) ->;
    method prepare_unmount @id(4) @rights(mount_control) ->;
    method shutdown @id(5) @rights(mount_control) ->;
    method lookup_target @id(6) @rights(mount_control) @max_bytes(65536) {
        WalkContext walk @id(1);
        u64 path_size @id(2);
        bytes<4095> path @id(3) @inline @length_field("path_size");
    } -> {
        NodeKey parent_dir @id(1);
        NodeKey node @id(2);
        u64 name_size @id(3);
        bytes<255> name @id(4) @inline @length_field("name_size");
    };
};
```

**路由方法与返回值语义。** 各方法返回 endpoint/binding 的绑定取值逐一定义如下（`resolve_absolute` 可返回 File 或 Directory，`derive_chroot` 返回派生 binding）：

- `resolve_absolute(walk, path)` 以本 binding 的 `visible_root_node` 为起点解析 absolute path 或 absolute symlink 目标。`walk.remaining_symlinks` 在每一次 symlink 展开（无论发生在 worker 还是 `vfsd`）时递减，耗尽即返回 `ELOOP`。返回对象可为 File 或 Directory：Directory 绑定 `{visible_root_node = 本 binding.visible_root_node, current_node = 解析终点}`，File 为常规 open description endpoint；两者的 effective rights 取本 binding.rights 与目标节点许可的交集。
- `enter_child_mount(NodeKey)`：仅当 `{parent_mount = 本 mount, mountpoint}` 命中 topology 时成功，返回**子文件系统**上绑定 `{visible_root_node = 本 binding.visible_root_node（保持不变）, current_node = 子根, mount_stack = 压入本挂载链标记}` 的 endpoint。进入 mount **不重置可见根**：绝对路径解析与 `path`/`getcwd` 的重建都仍以原可见根为锚，由 topology 提供下行穿越与上行拼接（§6.3）；这与 chroot 派生 binding（可见根收窄、`..` 截断）构成明确二分。`ENOENT` 只表示该本地节点没有 child mount，其他错误均为路由失败。
- `route_above()`：仅当 `current_node == 本地文件系统根` 且本 binding 的 `mount_stack` 非空时合法（否则 `EINVAL`）；返回**父文件系统在挂载点处**的 endpoint，绑定继承父侧上下文：`{visible_root_node = 本 binding.visible_root_node（保持不变）, current_node = 挂载点 NodeKey, mount_stack = 弹出一层}`。两个方向的 endpoint 分别缓存在 mount record 的 `root_anchor` 与 `parent_anchor`（generation-tagged）；缓存失效（unmount、media failure、peer close）后按需重建，语义始终以 `vfsd` topology 为准。
- `derive_chroot(NodeKey)`：为同 worker 内的子树创建派生 binding（`{visible_root_node = current_node = subtree_root, mount_stack 清空为 chroot 边界标记}`），worker 随后在本地物化绑定它的 endpoint，无需额外 RPC。这是 `NA_DIRECTORY_OPEN_FLAG_CHROOT` 的唯一实现路径；派生 binding 在可见根上的 `..` 原地截断且绝对路径解析以子树根为锚——**可见根收窄是 chroot 独有行为**，mount 进入永不触发。

**WalkContext 是全局预算。** 公共 `Directory`/`File` schema 不携带 walk 状态：预算由第一个需要跨 worker/vfsd 路由的一跳按配置常量初始化（symlink 展开上限；mount 穿越深度不占 wire 预算，由 `vfsd` 在 `enter_child_mount` 处按配置上限拒绝），此后按值随每一跳传递，只减不增。由于 mount topology 是森林（v1 禁止 stacking，无环），任何跨 worker 循环至少包含一次 symlink 展开，全局预算保证确定性 `ELOOP` 终止；单 worker 内循环同样被该计数终止。

**变更事务取代逐次检查。** rename/link/symlink/create/mkdir/unlink/rmdir 在产生任何副作用前调用 `begin_mutation`，其四个节点字段与两个组件名覆盖全部受害对象：`old_parent`/`old_target` 加 `old_name`（源父目录、被移动/删除/链接节点及其现名）、`new_parent`/`new_target` 加 `new_name`（rename/覆盖型/create/mkdir 的目标侧及目标名；create/mkdir 类 `old_*` 为全零，unlink/rmdir 类 `new_*` 为全零）。双 dirfd 形式的 `rename_at`/`link_at`（USERSPACE ADR §5.3）复用同一预留形状，仅多一步 instance 一致性预检。规则：

1. worker 必须在自己的元数据锁内完成 `begin_mutation` → 本地提交 → `ticket.commit` **提交**全程（r7：锁须持有到 commit 调用被 vfsd 受理、ticket 进入 `COMMITTING` 为止，而非仅到调用发出——否则超时可在本地副作用已发生、预留却被释放的窗口内触发）；`abort` 仅在本地提交前合法（规则 4）；
2. `vfsd` 原子校验：namespace_generation 未变；四个 NodeKey 均属于发起 worker 自己的 mount instance（否则 `EINVAL`）；受影响的每个 `(parent_dir, 组件名)` 都不是且不位于将被修改的 mountpoint 之下，且不与进行中的 prepare_mount/unmount 预留冲突（否则 `EBUSY`）。随后在预留表中锁定它们并签发 `PREPARED` ticket；
3. `prepare_mount`/`unmount` 与 mutation 共享同一张预留表，因此检查与底层元数据提交之间不可能替换或新建挂载点；
4. **MutationTicket 是预留租约，不是两阶段提交。** 本地元数据提交（规则 1 内完成）是唯一线性化点；`commit`/`abort` 的唯一效果是归还预留（`COMMITTED`/`ABORTED`）。r7 起 MutationTicket 与 MountTicket 共享 `COMMITTING` 态（§6.2 state 表）：vfsd 受理 `commit` 的瞬间即转 `COMMITTING`，过期计时停止、`EXPIRED` 不可达——这消除了"本地已提交、预留却因超时被释放"的竞态；worker 在 `COMMITTING` 期间崩溃或断连视为崩溃等价场景，预留照常释放、已生效的本地变更保持（`vfsd` 侧没有可回滚镜像）。`abort` 仅在本地提交前合法：一旦产生本地副作用，worker 只能 `commit`，需要撤销时发起新的补偿变更。`commit` 返回 `OUTCOME_UNKNOWN` 时 worker 不得重复本地提交，只能调用幂等 `status` 确认预留已归还后再继续；规则 1 的持锁要求保证"本地提交与归还之间崩溃"窗口不超过一次本地提交的时长，遗留预留由有限过期时间回收（§8）。

**MountControl 是 vfsd→worker 的绑定与生命周期入口。** r7 起 `bind_root` 不再参与 commit 验证（根 binding 由 commit 携带的 `{root NodeKey, generation}` 在 vfsd 侧创建，root_anchor 经 `bind_node` 播种）；它保留用于 root_anchor 缓存失效后的重建。`bind_node(node, flags, binding)` 由 `vfsd` 调用：MOVE 入一个新建 binding，要求 worker 物化该节点上绑定它的 Directory endpoint——`parent_anchor`/`root_anchor` 的重建以及未来管理面扩展都经此方法；v1 flags 只接受零。`lookup_target` 供 `prepare_mount` 的目标解析使用（§6.2；commit 前重验已删除），仅对 COMMITTED mount 发起，在 worker 元数据锁内原子执行。其余 sync/prepare_unmount/shutdown 沿用原生命周期语义。

每个已挂载 source 是一对进程与控制对象：

```text
vfsd mount record
{ parent_mount, mountpoint NodeKey, child_mount, control_client,
  parent_anchor, root_anchor, namespace generation,
  mutation/mount 预留表项 }

worker mount instance
{ unique BlockDevice lease, backend kind/state, local root NodeKey,
  mount_control server, MountTicket client,
  活跃 binding/endpoint/ticket 计数 }
```

`fat32d`/`exfatd` 本地处理本 filesystem 内的 lookup、read、write、metadata、目录 cursor、open description、journal/recovery 和 block I/O。普通 File/Directory endpoint 从 worker 直接返回 application；因此大量写入同一分区的路径为 `application → worker → LBD`，不经过 `vfsd`，也不触发 binding 路由。

worker 侧规则汇总：

- worker 在每个成功的本地 component lookup 后，按需经 binding 查询该 NodeKey 是否为 mountpoint；命中时返回 `root_anchor`（必要时重建）。不命中的查询可由 generation-tagged mountpoint cache 优化，但语义以 `vfsd` topology 为准。
- worker 在自己的文件系统根执行 `..` 时：binding 的 `mount_stack` 非空则调用 `route_above`，为空则原地截断；chroot 派生 binding 恒为后者。
- 跨不同 mount instance 的 rename 或 hard link 由 worker 直接以 `EXDEV` 失败（先比较两端所属 mount identity，无需 RPC）；覆盖 mountpoint、unlink mountpoint 或修改必要路由状态的变更因预留表以 `EBUSY` 失败。
- `Directory.set_current` 与 `Directory.set_root` 不用于 user-service endpoint；root/cwd 仍按前序 filesystem ADR 的规定，保存在 runtime bootstrap state。

这三条私有协议使用 NaoIDL 生成 binding，但不属于 public SDK ABI。它们可以随 `vfsd` 与 worker 同步升级；任何会影响 application 可见的 `Directory`/`File`、LBD ownership、预留语义或 mount lifecycle 的变化仍须经过本 ADR 的 ABI review。

## 7. 生命周期、顺序和恢复

1. kernel 启动 `ramdiskd`，不传 block capability；`ramdiskd` 创建 backing store 和用户态 factory。若需要解析分区表，manager 先取得整盘策略 view、读取后关闭，再按分区分别 acquire。
2. 受信任的 mount manager 请求 `vfsd` 在已验证 target 上创建 `MountTicket`。
3. manager 启动选定 worker，并通过用户态 block service MOVE 该 LBD；`MountControl` server end 与 `MountTicket` 仍按 mount bootstrap contract 转移，kernel 只校验 transferred resources 的 protocol metadata。初始 `NamespaceBinding` 不预发。
4. worker 初始化格式、开始服务 `Directory`/`File` 与 `MountControl` 后提交 ticket；`vfsd` 发布 mount topology，再经 `bind_node` 取得绑定初始根 binding 的根 endpoint 并保存 root anchor。此后普通 File `sync`/read/write 直达 worker；admin `Vfs.sync` 经 control 连接同步一个 mount instance，持久 write 在下层使用 `BlockDevice.flush`。
5. 正常 unmount 时，`vfsd` 停止新的跨 mount route，`MountControl.prepare_unmount` 检查 direct endpoint、活跃 binding 与未结 ticket，随后 sync、shutdown、原子移除 topology 并失效 anchor 缓存；worker 退出并关闭 LBD lease。worker crash 时 control client `PEER_CLOSED`，`vfsd` 标记 mount 失败并让未结 ticket（`MountTicket` 与 `MutationTicket`）进入 `EXPIRED`；既有 direct endpoint 也随其 peer 关闭而失败，绝不重绑。

每个 worker 必须串行化自己的 metadata 与 data ordering；`vfsd` 只串行化 namespace topology，不重排 block request，也不制造 writeback。`media_generation` 的变化对已挂载 source 是致命的：`vfsd` 标记该 mount 失败，后续操作确定性失败，该 mount 的全部未结 ticket 立即进入 `EXPIRED` 并释放预留；恢复需要重新 acquire LBD、重启 worker 并重新 mount。若 `vfsd` 自身退出，它发出的全部 `Vfs`、`NamespaceBinding`、`MountControl` client end、`Directory`、`File`、`MountTicket` 和 `MutationTicket` endpoint 都变为 `PEER_CLOSED`；kernel 既不重建其 mount table，也不静默重绑任何 endpoint。恢复根 namespace 是受控的 system-service restart/boot 策略，不是本协议的 retry 行为。

## 8. 硬上限与背压

初始值是配置默认值而非稳定 ABI；但下列上限必须从 `BlockInfo` 查询，或在 admission 前执行：

| 资源 | 必需上限 |
| --- | --- |
| block request 字节数 | `max_transfer_bytes`，绝不超过 MemoryObject 硬上限 |
| 每 request 块数 | `max_transfer_blocks` 和不会溢出的字节乘积 |
| in-flight I/O | 每设备 `max_in_flight`，外加 kernel 全局 pinned/bounce quota |
| 活动 LBD lease | 每个用户态 block manager 的非重叠区间数与全局 service quota |
| worker backend state、direct endpoint 与 VFS route record | 固定的每 mount 及全局 quota |
| pending mount ticket | 固定的全局 quota 和有限过期时间 |
| pending mutation ticket | 每 mount 固定 quota 与有限过期时间；被预留节点计入全局上限 |
| 活跃 NamespaceBinding | 每 mount 与全局固定 quota |
| mount 深度 | 有限的配置上限（拓扑为森林，v1 禁止 stacking，target 被占用即 `EBUSY`） |
| path/component 和 symlink expansion | 新协议统一 4095-byte 无 NUL path 上限（现存 Directory/File 方法仍为 `bytes<4096>` 字段，语义相同）；component/symlink 数量上限有限 |
| IPC | object-call ADR 既有的每 endpoint bytes/resources 与 invocation 上限 |
| worker bootstrap 转移 | MountControl、MountTicket 等 mount 对象经 bootstrap channel 消息资源转移；LBD 由 block manager 的 service RPC 取得；NamespaceBinding 在 commit 后由 vfsd 播种 |

全部 quota reservation、buffer pin/copy 和 endpoint allocation 都发生在可见 mount 变更或硬件操作之前。禁止在持有 VFS topology lock 时等待 I/O、等待 worker lifecycle RPC 或复制无界 buffer。实现先 snapshot 所需状态，释放锁，调用 worker，随后仅在 namespace generation 仍有效时提交。

## 9. 安全要求

- Block manager 在 `BlockDeviceFactory.acquire` 及 BlockDevice dispatch 前验证 capability/service scope、revision、feature、method right 和区间不重叠性。kernel 不拥有 LBA 策略；它只在 MemoryObject、channel 和实际 driver capability 边界执行通用安全检查。worker 不会通过 BlockDevice 得到 raw MMIO、IRQ 或 physical-frame authority。
- block buffer 被限制在精确的 MemoryObject 区间。driver 不能借此作为 map 任意 caller memory 的隐式 authority。
- manager 只能在 `prepare_mount` 成功后将一份新取得的 LBD、私有 control endpoint 和 ticket MOVE 给新 worker。`vfsd` 不接受任意 `File`、裸物理地址或伪造 protocol scope；同一介质的重叠区间也无法同时取得第二份 LBD lease。
- VFS 在 server-side 执行 path containment。absolute path、`..`、symlink、trailing slash 和 mount-root 处理均不能逃逸 binding 的 `visible_root_node`。chroot 是传统路径视图而非安全边界：它不撤销既有 capability（ADR §9.7），只约束之后的路径解析；该行为是文档化契约并有对应测试。
- rename/unlink/rmdir/create 家族必须持有效 `MutationTicket` 预留才能提交底层元数据；`prepare_mount`/`unmount` 与 mutation 共享预留表，杜绝检查与提交之间替换挂载点。
- 发生 `OUTCOME_UNKNOWN` 后，变更性 block/mount 操作不自动 retry。mount manager 在恢复前必须检查 state 并获取新的 capability。
- mount metadata、backend kind、device opaque ID 和 media generation 仅用于诊断；它们不是 authorization token，也不得用作全局 object lookup。

## 10. 实施边界与验证

### 语言分配与交付边界

本 ADR 的协议同时生成 C++ 与 Rust binding（r5 起两侧均为完整交付面）：

- **Rust 是 v1 唯一的服务端 toolchain。** `vfsd`、`ramdiskd` 与 filesystem worker 的 server 实现（dispatcher 骨架、resource disposition validator、协议违规处理）以 Rust 交付。NaoIDL generator 必须提供 Rust server/runtime/validator 模板，作为 Phase 0 的前置交付项。
- **C++ 侧保留 client + mlibc 数据面承诺**：生成的 typed client binding 与验证矩阵 mlibc 行的 POSIX 行为；现有 C++ server/runtime 模板继续服务 kernel adapter 与过渡期 contract test。
- mlibc 与 Rust std 消费同一份生成的 wire/error manifest（USERSPACE_FILESYSTEM_ADR §5.3 第 7 条）。Phase 0 交付生成侧：manifest 同时产出 errno 映射表与 Rust `ErrorKind` 映射表；消费侧切换由各自 phase 门禁（USERSPACE_FILESYSTEM_ADR Phase 3 覆盖 mlibc/std 两侧）。现有 mlibc 与 std 的两处手写映射届时废弃；abi.h 的 `NA_STATUS_*` 常量保持输入事实源，其清单须先与 ADR §15.1 对齐（补漏 `NA_STATUS_IO_ERROR=16`）。


### Phase 0 — ABI 与数据平面前置条件

- 将四个 public schema（`Vfs`、`BlockDevice`、`MountTicket`、`BlockDeviceFactory`）及三个私有 schema（`NamespaceBinding`、`MountControl`、`MutationTicket`）、scope mapping（16–22）、UAPI 名称和 named-right table 加入 NaoIDL generator；生成 C++/Rust manifest 并运行 generator contract tests。
- NaoIDL generator 必须先行扩展，否则上述 schema 无法冻结：新增 `kernel_view` resource 类型（当前语法仅有 handle/client_end/server_end，`BlockDevice`/`BlockDeviceFactory` 的 KernelView binding 无法表达）；named rights 改为按协议声明且未知名一律报错（当前 `METHOD_RIGHTS` 是 terminal 专用扁平表，`disposition_rights` 会静默丢弃未知名的资源权限）；支持资源字段的方向性 memory-object 权限；新增 `@errors(...)` domain error set 标注并生成类型化结果（本文各散文段落给出的域错误码即对应方法 `@errors` 集的规范性来源，冻结时不得引入散文之外的错误码）；资源字段的 scope 名查无映射必须编译失败（当前 C++/Rust validator 对未知名直接跳过校验）；跨 schema UUID/scope 碰撞检测（当前不存在）；将 `@concurrent` 解析进 descriptor/manifest（当前注解被忽略）；`reserved` 方法的名称持久化进 manifest 并生成占位 UAPI 宏（当前仅保留 int 列表，名称丢失）。本条改造同时覆盖 C++ 与 Rust 两个后端：`rust_disposition_scope`/`rust_disposition_rights` 与 C++ 表同源化，未知名一律报错。
- 实现用户态 `ramdiskd` block manager、固定内存盘、driver queue、区间 lease table、protocol dispatcher 和 service discovery。kernel 不加入 `block_device`/`block_device_factory` kobject；BlockDevice buffer 路径使用已有 MemoryObject syscall/frame。
- 定义并 review `vfsd`↔worker 的私有 NamespaceBinding/MountControl/MutationTicket contract，以及 worker 的 FAT32/exFAT backend contract。
- SDK 对齐项：C++/Rust 双侧 typed MemoryObject handle 封装（map/read/write 与 rights 检查）、invocation deadline-wait 安全封装；`NA_STATUS_*`/execution outcome 常量改为 generator 共享产物，取代 abi.h/mlibc/std 三处手写映射。
- 冻结变更打包（单一变更同时落地，任何一步先行都会产生需返工的中间冻结）：上述全部 schema 文件与 directory/file revision bump；abi.h 全部新宏——right bit 13–26、scope 16–22、§6.1 feature/flags/state 宏、`NA_BOOTSTRAP_FLAG_EARLY_SERVICE=1u<<1`；boot archive 通过 `naos://service/rootdir/0` ServiceDirectory 资源发布，不再增加 bootstrap capability kind；ADR §18.5 全集生成物（含 per-method `protocol_metadata.json`、`protocol_abi_test`/`protocol_negative_tests` 生成物与 stale generated file 检测）；Rust 侧 `protocol_error`（负 errno）→`ErrorKind` 转换通道（当前完全缺失），其与 mlibc 的五处既有类别分歧以 mlibc 现值为基线在 manifest 中裁决（`RESOURCE_EXHAUSTED` 的映射单独评审）。

退出条件：生成的 contract test 拒绝错误 binding/scope/rights、错误 resource disposition、溢出、不支持 flags 和 stale manifest；NaoIDL 层产出确定性 C++ 与 Rust codec/dispatcher tests，并以 Test/Echo 协议的一个 C++ server × Rust client 进程级互操作 test 证明 wire 与 resource disposition 兼容；一个 C++ kernel test 证明已接纳 write 具有稳定 byte snapshot，且没有 completion path 阻塞 IRQ handler。

### Phase 1 — userland BlockDevice 纵向切片

- 实现一个用户态 emulated block backend 与 `BlockDeviceFactory` service；kernel 启动 `ramdiskd` 时只转发 worker executable，不签发 block object。
- 验证硬编码 bootstrap 成功/失败、`ramdiskd` 失败不破坏 archive-backed normal boot、整盘/分区 lease 互斥、相邻非重叠分区并存、lease close 后回收、对齐 read/write、只读介质、`acquire.flags` READ_ONLY 的 right 削减与写入类 admission 拒绝、queue saturation、flush ordering、discard feature gate、media-generation failure 与 close/cancel race。

退出条件：一个 C++ integration test 通过对象执行 durable write 加 flush 和 fresh read；负向路径不泄漏 pinned object、queued request 或 capability reference。

### Phase 2 — `vfsd` route 与 worker 挂载事务

- 将 `naos/src/usr/vfsd/` 扩展为唯一的 `Vfs` server：实现单一系统 namespace、prepare/commit MountTicket flow、mount topology、NamespaceBinding/MountControl、共享预留表、anchor 缓存与 mount accounting；启动 worker 并向其 bootstrap LBD。
- mlibc 的 mount compatibility edge 只能连接 `Vfs`，绝不连接 kernel VFS syscall。

退出条件：mount failure 原子完成；path traversal 遵守 mount 与 `..` 边界；active direct file/directory endpoint 或 child-mount reference 导致 `EBUSY`；cross-source rename 返回 `EXDEV`；跨 worker symlink 循环由全局 WalkContext 确定性 `ELOOP` 终止；检查与提交之间的挂载点替换被预留表拒绝；`commit` 出现 `OUTCOME_UNKNOWN` 后 `status` 对账正确；covered target 的 `prepare_mount` 返回 `EBUSY`；application 不发现 worker，但命中 mount 后的普通 I/O 可直接到达该 worker。contract test 同时覆盖：`lookup_target` 的 TOCTOU 重验（commit 前 target 被 rename/删除必须放弃发布）、`begin_mutation` 的 NodeKey 归属拒绝、只读 mount 的无 flush 卸载，以及 topology lock 持有期间不等待 worker RPC 的断言（stress 下无界延迟检测）。

### Phase 3 — FAT32/exFAT worker 与 QEMU 持久化

- 实现可由 `vfsd` 启动的首个 format worker（FAT32 或 exFAT）及其恢复策略；按附录 A 特性矩阵，link/symlink/chmod 等不支持的操作必须确定性返回 `EOPNOTSUPP` 并纳入 contract test，不得静默降级。
- 打包 QEMU disk image，通过 manager mount，写入文件，`fsync`/unmount，重启后从新 mount 验证内容。

退出条件：serial output 可识别 `ramdiskd`、`vfsd`、worker、mount generation 和受控 failure；default boot 仍不依赖 optional disk；不恢复 `kernel/fs/vfs`、`ramfs`、`rootfs` 或 `pipefs` 下的任何 source。

## 11. 验证矩阵

| 层级 | 必需证据 |
| --- | --- |
| NaoIDL | deterministic manifest、UUID/scope collision check、resource/bounds decoder negative 与 rights table coverage、C++ server × Rust client 进程级互操作 test |
| userland block manager | alignment/overflow、read/write、FUA/flush ordering、queue limit、lease close、cancellation 和 cleanup Rust/UDS test |
| VFS | atomic mount rollback、symlink/mount-root containment、`..`、namespace generation conflict、`EBUSY`、`EXDEV`、worker peer-close、NamespaceBinding 路由（含 anchor 重建）、全局 symlink 预算、mutation 预留竞争、ticket `OUTCOME_UNKNOWN` 对账、stacking `EBUSY`、direct endpoint 生命周期、root mount unmount 拒绝、sync error 后 mount 保持可用、chroot 前 open 的 fd 越界访问为文档化预期成功 test |
| filesystem worker | malformed metadata、allocation failure、格式专属 crash consistency、flush propagation、media-generation failure、特性矩阵负向路径（`EOPNOTSUPP`）与大量 direct write 不触发 binding 路由 |
| mlibc | 启用时的 `mount`/`umount` compatibility mapping、`fsync`、`stat`、`getdents`、cwd/chroot 行为和无 raw block fd 时的 error；revision 2 后加 `lstat/fstatat(NOFOLLOW)`、目录 fd 上的 `fsync`、双 dirfd `renameat`（同 instance 成功含 FAT32/exFAT，跨 instance `EXDEV`）与 `linkat`（同 instance 成功限定支持 hard link 的 backend：RAM MVP 或 ext2；FAT32/exFAT 同 instance 返回 `EOPNOTSUPP`，跨 instance `EXDEV`） |
| Rust std | 与 mlibc 行等价的同一文件操作集经生成的 naos-idl binding 验证：metadata/symlink_metadata、rename/hard_link（hard_link 成功用例限定支持 hard link 的 backend，FAT32/exFAT 断言 `EOPNOTSUPP`；跨 mount `EXDEV`）、sync_all、错误 `ErrorKind` 映射与 manifest 一致 |
| QEMU | optional disk cold boot、format/mount/write/flush/unmount/reboot/read 场景；failure path 保留正常 archive-backed boot |

test 必须按需通过生成的 C++ binding、真实编译和 QEMU 验证行为。不得添加 Python source-scanning test 来证明 kernel VFS 或 hidden fallback 不存在。

## 12. 风险与决策

| 风险 | 决策 / 缓解 |
| --- | --- |
| VFS 变成另一个全能 filesystem | VFS 只拥有 namespace/topology/context；worker 拥有 format/data，BlockDevice 拥有 I/O。 |
| direct worker Directory endpoint 绕过 mount 规则 | worker endpoint 必须绑定 vfsd 签发的 NamespaceBinding；跨 mount、根 `..`、absolute symlink 与 mountpoint mutation 都经该 binding 的预留与路由。 |
| chroot 被误当安全边界 | 文档化为传统路径视图：只约束之后的路径解析，不撤销既有 capability；强隔离另立 sandbox PRD。 |
| 将 `Directory.open()` 误当作安全 walker | worker 对本地 component lookup/open 保持原子；跨 mount 路由经 NamespaceBinding。 |
| 可变 MemoryObject 在 write DMA 期间变化 | admission 时 snapshot 或 pin；有界 bounce allocation 在 dispatch 前计费。 |
| 将 flush 成功误认为持久化 | 显式定义 device-cache/FUA/flush ordering，并验证 reboot persistence。 |
| device name 变成环境 authority | 没有 `BlockDevice.open("/dev/...")`；discovery 与 policy 留在 manager。 |
| service restart 令 fd 静默指向新对象 | 既有 wrapper 失败关闭；恢复要求显式 reconnect 与 remount。 |
| 阶段膨胀到 pager/DevFS | 它们仍是具有独立 capability/lifetime review 的独立 PRD。 |

## 13. 后续工作

首个 issue 有意保持狭窄：新增用户态 `ramdiskd` block manager，并使用内存 backing 添加生成的 `BlockDevice.get_info/read/write/flush` contract test。它必须在不 mount filesystem 的情况下验证 buffer-range arithmetic、lease policy、in-flight backpressure 和 flush ordering；kernel 只验证已有 MemoryObject/channel capability 边界。

## 14. 附录 A：文件系统特性矩阵

本 ADR 不承诺“所有 POSIX 文件语义”；每个 backend 的能力由下表约束，未列出者按 POSIX 缺省并随具体格式 worker 的 PRD 扩展。

| POSIX 操作 | MVP RAM backend | FAT32 | exFAT | ext2（建议首个 inode FS） |
| --- | --- | --- | --- | --- |
| lookup/read/write/目录游标 | ✓ | ✓ | ✓ | ✓ |
| mkdir/rmdir/unlink/rename（同 FS） | ✓ | ✓ | ✓ | ✓ |
| hard link（维护 st_nlink） | ✓（RAM inode） | ✗ `EOPNOTSUPP` | ✗ `EOPNOTSUPP` | ✓ |
| symlink/readlink | ✓ | ✗ `EOPNOTSUPP` | ✗ `EOPNOTSUPP` | ✓ |
| 跨 mount rename/link | `EXDEV` | `EXDEV` | `EXDEV` | `EXDEV` |
| chmod/chown/access 检查 | 单用户 uid/gid 0 存根 | chmod/chown `EOPNOTSUPP`；access 按 uid/gid 0 存根放行 | 同左 | ✓ |
| fsync 持久化 | 明示不持久（重启丢失） | flush/FUA 映射 | 同左 | ✓ |
| 时间戳精度 | ns 存根 | 2s/10ms（FAT 规格） | 10ms–2s | ns |
| st_dev/st_ino 唯一性 | 单一伪 device | mount 派生伪 device + 本地 inode | 同左 | 同左 |

规则：

- 软链接目标字节存于具体 worker；相对链接在 worker 内解析，绝对/跨 mount 链接携带同一全局 WalkContext 经 binding 回 `vfsd`；`readlink` 不跟随。
- `lstat` 与目录 fsync、双 dirfd 原子 rename/link 由 Directory revision 2 的 `stat_node`/`sync`/`rename_at`/`link_at` 承载（USERSPACE ADR §5.3），所有列出的 backend 必须实现或确定性返回矩阵中的错误码；mlibc 与 Rust std 两侧同步交付。
- 硬链接只能由同一 filesystem worker 在自己的 metadata transaction 内实现并维护 st_nlink；`vfsd` 只确认两端是否同一 mount，跨 mount 必须 `EXDEV`。
- FAT32/exFAT 没有原生 inode、hard link、symlink、UID/GID 或 POSIX permission：link、symlink 必须显式返回并测试 `EOPNOTSUPP`。若产品要求这些语义，首个持久 worker 应改为 ext2/NaFS 这类 inode filesystem。
- 用侧车/overlay 在 FAT 上模拟 POSIX 语义必须另立 PRD，定义持久格式、崩溃恢复和外部 FAT 工具并发修改后的行为。
