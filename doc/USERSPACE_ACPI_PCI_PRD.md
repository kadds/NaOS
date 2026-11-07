# PRD：用户态 ACPI、AML 与 PCI 平台服务

- 状态：In Progress
- 日期：2026-09-19
- 适用范围：NaOS x86-64 kernel、native capability ABI、Rust 系统服务、Rust ACPI/AML/PCI crate 集成与 PCI 驱动模型
- 相关文档：[总体架构](ARCHITECTURE.md)、[Capability Handle 与异步 Invocation IPC](OBJECT_CALL_ADR.md)

本 PRD 的正式 QEMU 验收启动参数使用 512 MiB guest RAM；较早的 2048 MiB 跑测记录保留为历史事实，不能视为 512 MiB 配置下的复验结果。
通用 `util/run.py` 只转交调用者指定的内存参数，不替正式验收策略拒绝其他值。

## 1. 决策摘要

NaOS 将把完整的 firmware 描述解析和平台策略移至用户态。第一阶段部署为两个 Rust 进程：中性的 `platformd`
包含 ACPI backend（AML executor 与 ACPI policy modules）或未来的 Device Tree backend；`pcied` 是独立的 PCI-fabric
authority。**系统中不存在名为 `acpid` 或 `amld` 的进程或服务。**这两个名字至多是 `platformd` 内部模块的历史称呼；
它们共享唯一 AML namespace 和同步求值上下文。`pcied` 单独成进程，是因为它拥有不同且更危险的 PCI 重配置权限：

```text
kernel ── FirmwareRoot / ACPI executor ──► platformd
                                           └─ ACPI backend: executor + policy modules
kernel ── PciDiscoveryFabric ──► pcied
                                └─ local read-only PCI tree
```

ACPI backend 的 executor module 是唯一的 Rust AML interpreter 和 ACPI namespace owner；policy module 负责 ACPI
策略、缓存与生命周期。二者间调用是进程内 Rust API/有界任务队列，不是 RPC。`platformd` 对外发布 firmware-neutral 的
`PlatformPciTopology` 与 `PlatformPowerControl` 服务；二者分别承载 PCI facts/operations 和 policy-routed system power requests。
`pcied` 发现 PCI hierarchy、实施 PCI 资源策略，并只向驱动授予单一 PCI function 的 capability；它消费版本化的平台事实，
不维护或调用 AML namespace，也不解析 ACPI 或 Device Tree bytes。

通用 PCI policy/driver binding 仍未闭环；当前生产 catalog 仍固定为 Virtio Block。**`pcied` 读取 `PlatformPciTopology` 的 root facts，计算其与 MCFG 的
intersection，并请求 kernel mint 一个 owner-bound `PciFabricAdmin`；它仅映射该 capability 固定范围内的只读 ECAM，
再为扫描到的 BDF 建立 owner-bound `PciFunction` capability。**`PciFunction` 不提供通用 config write、任意 BAR/MMIO 或直接 DMA；**
BAR 仅可通过 kernel-minted per-device `_CRS` evidence 调用 typed `open_bar(index)`，mapping 仍是受限的 MemoryObject view。
它也可通过 typed `route_interrupt` 请求 kernel mint 一个 owner-bound `InterruptSource`，并另行请求受所选 IOMMU backend policy 限制的
`PciDmaDomain`。这些 capability 的 owner 是创建它们的 process：正常启动的 `pcied` 将匹配 Virtio Block function 授给
`virtioblkd` child，由 child 创建并持有 DMA domain；opt-in QEMU EDU smoke 也将 attenuated `PciFunction` 授给 child，由 child
创建并持有 DMA/interrupt resources。内核现提供 typed
`PciFunction.grant_driver` primitive：调用者传入
Process capability 与 method/BAR profile，返回预置单个 one-shot function locator 的有界私有 `ServiceDirectory`；driver 可在该私有目录
注册服务 endpoint，父 `pcied` 持有 observer handle，注册不会进入全局服务目录。grant 不接收裸 PID、BDF 或任意 URI 授权请求。
生产 loader 目前只服务固定 Virtio Block binding，不是通用第三方 driver manager；opt-in EDU smoke 另外使用测试专用 child
binding 验证 route 权限。普通启动为精确匹配的 Virtio Block function 调用 deferred prepare → per-function grant → start：child
只获得 `read_config`、`route_interrupt`、DMA-domain 和 capability 链所需 BAR 权限；
并在 private ServiceDirectory 提供 BlockDeviceFactory。catalog 仅在 factory protocol metadata/rights 验证通过后登记，并在 child
exit 时移除 entry。生产 catalog 目前只含 Virtio Block transitional/modern PCI IDs；这不是通用第三方 driver ABI，也没有自动重启、
热插拔重扫、power 或 trusted-driver override。2026-10-24 的 headless Q35 + Intel VT-d QEMU smoke 已通过该 child 的 MSI-X
route、`BlockDevice.write`、`flush` 和 `read` 往返；这只验证一个 modern Virtio Block function，不代表真实硬件、AMD IOMMU 支持
或完整设备故障/重启生命周期已经验收。AMD-Vi 是本期目标：ACPI 启动快照现在解析 IVRS，并保留 IVHD/IVMD 的规范归属和格式选择；
另有 AMD default-deny/translated-domain DTE 和 Mode=4/48-bit 四级 4 KiB page-table builder/entry encoder，已有 C++ 验证；
full-width 2 MiB default-deny table initializer、AMD command encoders 和 IVHD requester resolver 已补入。AMD controller 现在具备独立的
IVMD fail-closed admission、DTE/command/event buffer setup、default-deny activation、有界 command completion、event-log drain/record
decoding，以及 standalone Mode=4 DMA domain attach/map/unmap/detach 与 DTE/IOTLB invalidation。ACPI 启动现已按 IVHD 能力计算
16 KiB/512 KiB aperture，为选中的 AMD units 建立 exclusive resource lease 和 uncached kernel mapping，并在 PCI DMA quiescence、IVMD
policy 与 MADT IOAPIC source 校验通过后初始化 controller、激活 default-deny；event polling 可启动时，另注册 DMA 与 per-requester PCI MSI/IOAPIC provider。
IVHD requester resolver 实现 PCI select/all/range 与 IOAPIC special-device source lookup；alias requester 在 runtime 能证明并约束完整
alias group 前 fail-closed。AMD basic IRTE、MSI table-offset message 与 DTE interrupt-control encoders 已补入；controller 现有固定上限的
requester table owner、512-entry IRTE 分配、DTE attach/detach、route-cookie 校验及 DTE/interrupt-table invalidation；首次挂接和最后撤销
遵循 AMD 48882 §2.2.2.2 的 abort -> table update/invalidation -> remap / detach 顺序，interrupt-remapping DTE word 使用单次对齐 64-bit
store。ACPI provider 将其接入 owner-bound PCI MSI 与 IOAPIC route install/retire。每个 requester 最多 512 个 basic IRTE，系统最多登记 256 个 requester
table owner。AMD PCI IOMMU capability header 与 PAsize 在建立 DMA domain 时校验；每个 unit 的 MMIO `01A0h[NumIntRemapSup]` 也会单独检查，
仅 512/2048 编码开放 PCI MSI/IOAPIC interrupt remap，reserved 编码只关闭该 unit 的 interrupt route 能力，不关闭 default-deny DMA。AMD provider 现经公共 router 接入 owner-bound `PciDmaDomain`
create/map/unmap/close、BME admission 与 `PciDmaFaultSource`；ACPI adapter 每 1 ms 有界轮询 AMD event log，unit overflow/drain failure
会 quarantine unit 并关闭对应 BME，source queue 溢出会 quarantine requester 并拒绝再次启用 BME。provider activation scope 明确为
per-requester：PCI MSI route 更新对应 DTE，使用 xAPIC interrupt address aperture 与 MSI data table offset；AMD IOAPIC route
仅接受 MADT 与 IVRS 均唯一授权的 special requester，并将 IOAPIC vector 作为 basic IRTE table index。首次安装将 pass-through DTE
切到 abort、发布并失效 table 后再启用 remap；退役在 RTE 保持 masked 时清除 IRTE 并恢复 IVRS pass-through DTE。HPET 仍使用
IVRS 授权的 unmapped pass-through DTE，或保持 source masked。公共 route lifecycle 在 retire 前后 drain 目标 vector。AMD IOAPIC
controller callback 与 requester-scoped router 测试现已执行；AMD controller/event parser、router、DMA source 和物理资源相关单测
均通过。512 MiB AMD-Vi EDU smoke 已验证 default-deny、authorized DMA、MSI remap、memory IOPF fault delivery、BME disable 与 owner teardown；
正向 fault gate 使用按 AMD 48882 修正事件编码的 QEMU 11.1.1 测试夹具。真实 AMD-Vi 硬件未测试，且不是本 PRD 的验收前置条件。
PCI MSI/IOAPIC route resolution、install、retire 和 interrupt-remapping activation 已经过 provider router，使用 provider 返回的编码与 opaque token；
Intel VT-d 注册 unit-global interrupt provider，AMD 注册 per-requester PCI MSI/IOAPIC provider；router 不会把 AMD DTE/IRTE 操作伪装成 global IRE。
DMA source resolution/create 经同一 router dispatch；Intel 与 AMD 均接入 DMA domain 操作及 PCI MSI/IOAPIC route 生命周期，fault/event 硬件操作由
Intel IRQ 与 AMD 有界 timer poll 投递 fault source。AMD IOAPIC route 已接入 provider install/retire 与 mask-first rollback；controller/router
fake-register 测试、共享 Intel/AMD backend conformance、512 MiB AMD memory-fault/BME/owner teardown QEMU gate 均已通过；Intel 与 AMD
现满足相同核心 DMA/MSI 合同。真实硬件验证尚未进行，不属于验收前置条件。
PCI tree 仍只保留在 `pcied` 进程内。

`platformd` 从 AML `_HID`、`_CID` package 中的每一项及 `_UID` 产生规范化设备标识，支持有界字符串与整数 EISA ID；
`PlatformPciTopology.list_devices` 以独立 kind 返回 HID、CID 和 UID；`pcied` 仅将 HID/CID 交给 compatible-ID driver matcher，UID 不参与驱动匹配。
driver matcher
先按 primary/additional compatible ID 选择，未命中时可按精确 PCI vendor/device ID fallback；PCI ID 不是 BDF 或权限选择器。
启动副作用前，
它会验证整份 catalog：拒绝重复 compatible ID、非 boot-module image URI、非规范 executable path、未知 function rights 和超出六个 BAR
index 的 mask，也拒绝重复 PCI vendor/device key；即使本轮没有设备匹配，损坏的 registry 也会令 preflight 失败。opt-in
QEMU EDU entry `QEMU0002 -> edud` 仍用于 grant/bootstrap smoke；普通构建另有精确 PCI-ID Virtio Block binding。driver loader
先对完整 function snapshot 建立稳定顺序的匹配计划，按镜像
分组且每个 one-shot image 只读一次；随后为整份计划准备 deferred child 并完成每个 function 的 grant，只有全部 image/grant 成功后
才开始任何 child。这样后续 image/grant 失败会 drop 尚未启动的 child，不留下部分运行设备。`Process.start` 的回复现在由
`PreparedEarlyService::start_confirmed` 通过 Tokio readiness 异步等待并返回；reply 未确认时最多重试一次，kernel 对已启动 child
也返回成功，所以重试不会重复调度线程。双 EDU smoke 另发重复 start 请求验证该幂等合同。全部 start reply 确认后，PCIED 并发收集 child exit
outcome。start/exit 失败会令整份 smoke 返回失败；已经启动的 sibling 仍会等待并报告，且不会被伪称为已回滚。
两个 QEMU EDU function 的 opt-in 集成现已验证同一 image URI 只解析一次、每个 function 独立 grant/启动/等待，并以各自 BDF 推导
VT-d requester ID。Virtio Block catalog 也使用稳定匹配计划与 one-shot image 分组；其它非 EDU 设备策略与 hotplug lifecycle 仍未完成。编入 opt-in smoke 的 `pcied` 若启动在不含 QEMU EDU 的 PCI fabric 上，
会跳过 EDU 专用测试动作并继续普通只读枚举；兼容 ID 歧义或已匹配设备的 grant/启动/DMA/MSI 失败仍然 fail-closed。

2026-10-24：新增固定 URI `naos://system/pci/block-device-catalog` 的 typed broker 合同。`pcied` 仅获得绑定到该 URI 的
ServiceDirectory provider right；驱动在 grant-private directory 中以固定 URI `naos://service/pci/block-device-factory`
发布 `BlockDeviceFactory` listener；catalog 经该私有目录连接 listener，以 `firmware_version + pci_fabric_generation` 校验分页快照，并只向 client 转交
UUID/scope/revision/features 全匹配且 rights 已收窄的 factory endpoint。broker 不转交 PCI function、MMIO、DMA、IRQ 或 driver
ServiceDirectory。PCIED 现在会为启动成功且 factory 身份验证通过的 Virtio Block child 登记 provider，并监听 child exit 撤销对应
catalog entry；目前只有启动期 provider handoff，没有设备热插拔重扫或 child 自动重启。opt-in block smoke consumer 已通过
catalog acquire、BlockDevice write/flush/read 与数据比对。真实硬件、生产 consumer 与完整 G11 生命周期仍未验收。

Virtio PCI driver 现消费 bounded capability-chain parser、volatile BAR transport、split queue 与逐页 `DmaRegion` helper。初始化只接受
modern transport，协商 `VERSION_1` 和 `ACCESS_PLATFORM`，读取稳定 capacity snapshot，只 grant/map common、notify、device BAR；ISR
capability 被解析但不需要映射。QEMU fixture 为 transport BAR4 和 MSI-X table/PBA BAR1 分别提供 `_CRS` authority，kernel 私下映射
table，child 只映射 transport BAR。驱动映射 descriptor/available、used、header、status 与双方向 data backing，再配置 queue 0 的 MSI-X
vector 0 并绑定 `InterruptSource`。BlockDeviceFactory 提供最多 256 个活动 LBA lease；overlap 或 lease table 满时返回 `EBUSY`。设备 offline 后 factory 与已有
BlockDevice endpoint 都返回 `ENODEV`。read/write 通过 servicekit 映射客户端 MemoryObject，
复制到/自方向受限 DMA buffer；flush 发送 Virtio flush request，FUA 以 write 后 flush 实现，discard 不宣称支持。目前单队列、单个
in-flight request、最多 128 个 512-byte sector；完成等待通过 Tokio readiness 同时等待 `InterruptSource` 与 DMA fault source，超时或
fault 会停 BME、尝试 reset 并将设备置为 offline；当前请求先收到错误回复，随后 child 以非零状态退出，PCIED 的进程退出观察器移除
对应 factory/catalog entry。不会自动重启设备。Opt-in QEMU smoke 已通过 factory 获取设备后写入 8 个 sector、flush，再读取并
逐字节比对，同时检查 VT-d interrupt-remapping trace。每次测试新建 build-local 32 MiB raw image，只写 LBA 8–15，不格式化启动盘
或系统 root image。该证据不代表真实硬件、AMD IOMMU 或其他 Virtio 设备已验证。

2026-10-24 续：kernel 现在只把 `pdrv-<token>` 命名的 boot module 发布为
`naos://service/boot/module/pdrv-<token>`，且只将 `util/run.py` 显式配置的这些 URI 加入 PCIED 的 scoped ServiceDirectory；普通服务仍无权读取 PCI
driver image。module 名受 15-byte boot-token 上限和 ASCII 小写字母/数字/`-`/`_` 校验。`util/run.py` 显式映射将 `pdrv-edud`
指向 `edud` 测试镜像，并将 `pdrv-vblk` 指向 `virtioblkd`；缺少的可选镜像会省略，陈旧 build 文件不会隐式进入 boot image。
`virtioblkd` 已加入 CMake Rust build/install graph。QEMU smoke 已验证该 token/URI、子进程 grant、catalog acquire 和 block I/O 路径：
历史跑测使用 2048 MiB、headless Q35、Intel VT-d、现代 `virtio-blk-pci`、`iommu_platform=on`，并通过 opt-in `_CRS` SSDT fixture attested BAR。
2026-10-25 已在正式 512 MiB 参数下重新验证：独立关闭 fault-injection 的 normal-mode build 通过 BlockDevice write/flush/read-back；
fault-enabled build 通过终止 DMA fault、EIO、provider/catalog removal、36 个 mapping 清理及 MSI route teardown；allocator-quota
opt-in smoke 也在 512 MiB 下通过并达到 shell startup marker。以上仍是 QEMU 虚拟平台证据，不替代真实硬件。

这不是把现有 kernel ACPICA 原样搬到用户态。内核保留 RSDP/table 的安全引导、启动所必需的最小结构化信息，
以及 MMIO、I/O port、PCI config 和 IRQ 的机制；namespace、设备策略和 AML 语义均属于用户态。

## 2. 背景与问题

现有 kernel ACPICA 已经初始化 tables，并由 OSL 直接读写 I/O ports，同时拥有固定电源事件和关机路径。
SystemMemory 与 PCI config OSL 尚未成为受控用户态接口。若直接给予用户态物理地址、全 I/O port 或任意
PCI BDF 访问权，则任何崩溃或被攻陷的服务都可以读取 RAM、改写内核/设备状态，或充当其他服务的权限代理。

AML 是固件提供的可执行输入。把它放到用户态可缩小内核攻击面和恢复范围，但不能神奇地把恶意固件变为安全：
一个能够合法操作平台电源、EC 或 chipset MMIO 的 AML 域本身仍有高硬件权限。本 PRD 的安全目标是将该域
限制到 `platformd` 的 ACPI backend，阻止其获得一般 RAM/内核内存，并防止其把该权限转交给 `pcied`、驱动或普通进程。

### 2.1 迁移前内核依赖审计（历史基线）

下表记录用户态 ACPI 迁移前的依赖基线，不描述当前 kernel 是否仍链接或调用 ACPICA；当前实现状态以本 PRD 后续进度记录为准。

当前 kernel 的 `arch::ACPI` 通过 ACPICA `AcpiGetTable()` 读取少数结构化表；它没有调用 DSDT/SSDT 中的 AML
方法来完成下列启动功能。直接调用点的结论如下：

| 表 | 当前内核用途 | 目前是否启动关键 | 用户态化后的归属 |
| --- | --- | --- | --- |
| RSDP、RSDT/XSDT | 找到 ACPI table root，当前供 ACPICA 初始化 | 是，作为 ACPI 启动入口 | kernel 保留最小发现、验证和 snapshot |
| MADT | Local APIC 地址 override、I/O APIC 地址/GSI base、IRQ source override、CPU APIC list | 是；`acpi=on` 时 I/O APIC/APIC 路由依赖它 | kernel 保留最小 MADT parser 和紧凑的 boot topology snapshot |
| FADT | ACPI PM timer 的 I/O/MMIO base 与计数器宽度 | 否；PM timer 无效时已回退 PIT，TSC/KVM pvclock 通常优先 | 过渡期 kernel 可保留；最终可由专门 clock/firmware service 提供受控 timer source |
| HPET | `hpet=on` 时取得 HPET base 并映射为 clocksource | 否；HPET 默认未启用且有其他时钟回退 | 过渡期 kernel 可保留；或由 kernel platform-clock descriptor 替代 |
| SRAT | 将 Local APIC ID 映射到 NUMA domain，构造 kernel CPU mesh | 对 NUMA 正确性重要，但不是基础单核启动必须 | kernel 保留最小 SRAT CPU-affinity parser；以后可扩展为 boot topology contract |
| FADT `BootFlags` | 8042 存在性 | 当前实际上未使用：实现固定返回 `true` | 可立即剥离该表依赖或恢复为受验证的最小字段读取 |
| DSDT、SSDT、其他 AML/设备表 | 当前没有 kernel 直接语义消费者 | 否 | 只以 immutable snapshot 交给 `platformd` ACPI backend |
| FACS | kernel 使用其中 32-bit Global Lock word 与平台固件仲裁 AML 资源访问 | 是；FADT 间接引用，Global Lock 还依赖 PM1 GBL_STS/GBL_EN/GBL_RLS | 验证并保留完整 FACS page envelope；只允许 kernel 对 GlobalLock DWORD 做原子更新，绝不作为普通 SDT 或用户态可写映射 |
| MCFG | 当前没有调用点 | 否 | 为 `PciFabricAdmin` 新增 kernel 最小 MCFG parser；不交给 kernel AML |

`ACPI::enable()` 当前只初始化 kernel typed fixed-hardware source；kernel 不再启动 ACPICA interpreter 或其 fixed-event
handler。旧的 QEMU/Bochs/VirtualBox 端口关机/复位路径已移除，`NA_SYSCALL_POWER_OFF` 也已从 ABI 删除；用户命令必须经过
`PlatformPowerControl` policy 服务。

结论是**可以剥离完整 ACPICA 和 AML**，但不能在用户态服务启动之前剥离 kernel 的早期 platform topology 消费者。
目标形态是一个小型、无 AML 的 `acpi_boot_tables` 层，而不是让早期 kernel RPC 等待 `platformd`：它只解析 RSDP/RSDT/XSDT、
MADT、SRAT，以及在保留相应时钟功能时的 FADT/HPET；MCFG 是 PCI capability 工厂所需的新最小解析项。其余 table 只复制、
验证和发布。

这个最小 parser 必须替换当前依赖 ACPICA table walker 的实现，而不是继续在 kernel 初始化完整 ACPICA 后声称 AML 已迁移。
它必须验证每一个 SDT/subtable 的最小 header length、`Length` 的边界与算术溢出、checksum 和重复/异常 entry；特别禁止以
`header = header + header->Length` 方式信任 firmware 长度字段，否则零长度或短长度 subtable 可令内核死循环或越界。

### 2.2 单一 AML interpreter 所有权

RSDP、RSDT/XSDT、MADT、SRAT、FADT 与 HPET 都是固定二进制数据结构，kernel 对它们的上述消费不需要 AML namespace、
AML method 或 ACPICA interpreter。最终 kernel 不得调用 `AcpiInitializeSubsystem`、`AcpiInitializeTables`、
`AcpiEnableSubsystem`、`AcpiGetTable` 或 `AcpiInstallFixedEventHandler`，也不应链接 kernel ACPICA OSL；这些调用会使
kernel 成为第二个 ACPICA owner，并可能竞争 SCI、fixed events、global lock 或 power registers。

FADT 具有 DSDT/FACS 指针。kernel 可以为 catalog discovery 读取并验证这些指针及其地址范围，但不得解析 DSDT AML。
DSDT 必须被复制为只读 snapshot，即使它未作为 XSDT entry 出现。FACS 不是普通带 checksum 的 SDT，含 Global Lock 与 wake
state。Global Lock 不通过 `AcpiFirmwareState`、MMIO capability 或用户态 mapping 暴露：`AcpiFixedHardware` 在 kernel 内保留
FACS `GlobalLock` DWORD 地址；AML `Acquire/Release` 只能经绑定到 interpreter execution thread 的
`AcpiResourceAuthority` typed method 请求原子仲裁。FACS 整页仍由 `firmware_state` ledger 保留，generic region lease 不得访问。
PM1 `GBL_STS/GBL_EN/GBL_RLS` 由 kernel fixed-hardware adapter 管理；SCI 上的 GBL_STS 必须先唤醒 kernel waiter，之后才能由
`platformd` drain/ack 其它 fixed/GPE event。无 FACS 或 FADT `HW_REDUCED` 时，kernel 使用单一 PID/TID lease 和 wait queue
执行软件串行化；AML `_GL_` 的递归深度由用户态解释器维护，host backend 只在外层获取和最终释放时调用 kernel，重复 lease 请求会被拒绝。FACS 存在且非 `HW_REDUCED` 时，OS 与固件共享 Global Lock，必须始终对 FACS word 做 CAS；GBL SCI 不可用时轮询该
word，已启用的 GBL event 路径运行中丢失时则 fail-closed。GBL_EN readback 只表明通知路径是否可用，不代表 FACS lock 不存在。

内核解析的 FADT 字段还必须形成资源保留表：正在由 kernel 使用的 PM timer I/O/MMIO range、HPET range、Local/I/O APIC
range 与 SCI routing 不可再授予 `platformd` ACPI executor 的 OperationRegion。这样即使没有两个解释器，kernel clock/interrupt machinery
也不会与 AML 同时改写同一硬件状态。

### 2.3 `FirmwareRoot`：跨架构 firmware 入口 capability

bootloader 向 kernel 交付 firmware 的物理入口；kernel 将其转换为一个 `FirmwareRoot` capability 并在启动 `platformd`
之前发布。`FirmwareRoot` 是唯一的用户态启动协议，不存在 `AcpiRootCap` 这种架构专属根能力：

```text
bootloader -> kernel-internal physical firmware root -> FirmwareRoot capability -> platformd

x86-64:  ACPI
ARM64:   ACPI 或 Device Tree
RISC-V:  Device Tree 或 ACPI
```

kernel 内部 backing 可以保留 physical provenance，例如：

```text
kernel-only FirmwareRootBacking
  = Acpi { rsdp_phys }
  | DeviceTree { dtb_phys, dtb_size }
```

但这不是用户 ABI。用户态可见的 `FirmwareRoot` protocol 只返回 format revision、不可拆分的 `firmware_version` 字符串和
`FirmwareObject` descriptors；`open_object()` 返回固定长度、只读的 `MemoryObject` snapshot。它不返回 `PhysAddr`，也不
接受 caller 给出的 physical address：

`firmware_version` 在 IDL 中使用不设长度上限的 `string`；具体 RPC 仍受现有 wire message 和 transport 容量约束。

```text
FirmwareRoot.describe() -> { kind: Acpi | DeviceTree, revision, firmware_version }
FirmwareRoot.list_objects(cursor, limit) -> { objects: [FirmwareObjectDescriptor], firmware_version }
FirmwareRoot.open_object(object_id) -> readonly MemoryObject
FirmwareRoot.list_relations(cursor, limit) -> { relations: [FirmwareRelation], firmware_version }
```

对于 ACPI，object directory 包含 RSDP、RSDT/XSDT、FADT 间接引用的 DSDT、SSDT 及其他已验证 table snapshot；对于
Device Tree，它至少包含 root DTB，后续可用相同机制表达验证过的 overlay。descriptor 可携带 format-specific 的
signature/type metadata，绝不携带可用于 map 的物理地址。ACPI table bytes 中出现的地址字段也只是未授权数据；只有
`FirmwareRoot` 已收录的对象才能被 platform backend 打开。

这个模型贯彻两层解析原则：kernel 只解析 firmware 到“让 kernel 自己活起来”所需的 MADT/SRAT/FADT/HPET 等最小信息；
`platformd` 对同一个 immutable generation 重新解析完整 firmware，以管理整台机器。两者读取的是同一 firmware 实例的
受验证 snapshot，而不是共同拥有一个可写物理 mapping，因此没有 AML interpreter 或内存权限冲突。不同 parser 得到的
平台结论可以独立记录和诊断，但用户态结果不得反向成为 kernel 早期启动的事实来源。

### 2.4 Rust crate 依赖与适配边界

kernel 可以、也应当完全移除 ACPICA。RSDP/root-table、MADT、SRAT、FADT、HPET 和 MCFG 的最小结构 parser 不依赖
ACPICA；`platformd` 的平台快照和 `pcied` 的 ECAM discovery 也不依赖它。因此“只做 boot topology 与只读 PCI discovery”的
阶段可以在整个系统完全没有 ACPICA 的情况下运行。

完整目标仍需要 AML interpreter，但不自行实现。`platformd` ACPI backend 采用
[`rust-osdev/acpi`](https://github.com/rust-osdev/acpi) 的 Rust crate，目前采用 `acpi = 7.0.0-pre1`、
`default-features = false`、features `alloc, aml`；它提供 static-table parser、AML interpreter、namespace、resource 与
PCI routing 支持。`pcied` 直接锁定 `pci_types = 0.10.1`，用于 PCI header/capability decoding；它不能获得 raw physical
mapping。`pci-driver` 的现有 backend 是 Linux VFIO，不适合作为 NaOS capability API，故不采用。

crates.io 的独立 `aml = 0.16.4` 也是 `rust-osdev/acpi` 仓库的较早拆分式发布：它以 `AmlContext`/旧 `Handler` API 与旧版
静态-table `acpi` crate 配合。当前锁定的 `acpi` 7.0.0-pre1 已将该职责集成为 `acpi::aml::Interpreter`，并不依赖独立
`aml` package。
ACPI backend **不得**同时引入这两条版本线；它们的 table、handler、namespace/value 类型与生命周期模型不同，混用会造成重复
interpreter 与不受控的适配层。NaOS 选择当前整合的 `acpi 7.0.0-pre1 + pci_types 0.10.1` 版本线。

上述版本必须通过 NaOS 的 pinned Rust 1.97.1 fork 编译验证后才可冻结到 `Cargo.lock`。当前 `acpi` 7.0.0-pre1 使用 Rust 2024
edition 与不稳定的 allocator API；若 NaOS stage1 不支持该 feature，优先与 upstream 协作或在受审计的
`rust-acpi-naos` fork 中最小化移除该 toolchain 依赖，**不得**回退到 kernel ACPICA 或自行重写 AML parser。

`acpi` crate 的上游 `Handler` 假定宿主可按任意 physical address map memory，并同步执行 raw memory/I/O/PCI access；这与
NaOS 的 capability 模型不兼容，不能原样实现。`platformd` ACPI backend 使用锁定 revision 的 `rust-acpi-naos`
security-boundary fork，而不是 fork 一个新 interpreter：

1. table mapper 只把 DSDT/SSDT/FADT 指针解析为 kernel `FirmwareRoot` 已收录的已验证 snapshot；未知 firmware address
   一律失败，绝不成为 physical mapping authority；
2. OperationRegion adapter 在定义/首次使用时请求受限 `IoPortLease`、`MmioRegion` 或 `PciConfigSpace`，随后走本 PRD 的
   TSS/PTE 直接数据面；`SystemIO` 使用带 generation、device 与 execution-thread binding 的 TSS port lease，CPU 对该
   port range 同时允许 `in/out`，解释器 access token 继续执行当前只读 profile；
3. adapter 把 table/mapping/region 失败转换为可恢复的 Rust error 与 `platformd` domain error，不因 malformed firmware 在
   kernel 解引用指针或 panic；
4. adapter 是唯一能接触 NaOS capability binding 的代码。上游 crate、其他 `platformd` module、`pcied`、driver 和 public NaoIDL 都不
   暴露 handle、物理地址或 mapping 指针。

FADT/DSDT 的原始字节必然含 firmware physical address；catalog descriptor 不主动发布这些地址，但 ACPI backend 读取 table bytes
后仍可能看见它们。它们仅是未经授权的数据，不能用于请求任意映射。适配层必须按 catalog identity 与完整范围做二次解析，
而不能仅相信地址数值。

Rust crate 的选择不等于宣称所有真实固件兼容。合入前必须在 upstream test suite、QEMU/OVMF tables 和匿名 table corpus
上验证 `_STA/_CRS/_PRT`、OperationRegion、异常长度、递归/循环、取消与 resource bounds；真实硬件测试不是验收门槛。对 unsupported
AML，`platformd` 必须报告 degraded/unsupported；不得自动启用第二个 ACPICA interpreter 作为隐式 fallback。

## 3. 目标和非目标

### 3.1 目标

- 内核发现 firmware root，并向 `platformd` 提供长度、边界和校验已经验证的只读 `FirmwareRoot` snapshot。
- ACPI backend executor 加载 DSDT/SSDT，建立一个全局 namespace，并执行受策略约束的 `_REG`、`_INI`、`_STA`、`_CRS`、
  `_PRT` 等方法。
- 所有硬件访问均由 process-local capability 表达，并在接纳请求和实际数据访问两处执行校验。
- `pcied` 使用经核实的 root bridge/ECAM 范围枚举 PCI，驱动只拿到受限的 function、BAR 和 IRQ 权限。
- 进程崩溃、取消、超时和 service restart 的结果可观察，且不会被误称为硬件操作回滚。
- 所有 stable RPC protocol 均由 NaoIDL 定义；本文是这些 schema 的规范性设计输入，未冻结 method ordinal。

### 3.2 非目标

第一阶段不支持 ACPI hotplug、S3/S4、EC、动态 NUMA 重配置、PCI BAR 重分配、桥配置写入，或对生产系统开放
任意 AML 方法求值。PCI config discovery 始终只读；typed BAR mapping 只开放经 device `_CRS` 和 root window 双重约束的
assigned BAR，不改写 BAR、bridge bus number 或其他 PCI config register。

当前已实现 `platformd -> pcied` 的 `PlatformPciTopology` root/generation 路径、kernel-minted `PciFabricAdmin`、
`PciFunction` identity capability，以及 generation-scoped `list_devices`/`get_resources` 只读快照 API。设备身份在该
边界被归一化为带 HID/CID/UID kind 的设备标识；AML namespace path 不出现在平台服务 wire 上。`PciFunction.read_config`
只允许对 capability 绑定的 BDF 做自然对齐的 8/16/32-bit 标量读取；request 不携带 BDF，kernel 串行化 ECAM 访问，且不
返回 ECAM mapping 或任何 config-write authority。
Phase 6 的 G11 已有首段 kernel contract：owner-bound `PciFunction.create_dma_domain` 只在启动期 PCI DMA quiescence、
所选 Intel VT-d 或 AMD-Vi backend 的 default-deny 状态有效、且 firmware reserved-memory policy 已满足时 mint `PciDmaDomain`；
其 typed RPC 只映射完整、已就绪、归内核所有的
`MemoryObject` 页，要求先停止 BME 才能 unmap，且 BME enable 只能经已 ready 的 IOMMU domain。无 IOMMU 时当前 factory
fail-closed。单个 PCI function 的 `_CRS` resource attestation、root-relative `_ADR` 到 BDF 的 kernel 校验，以及 typed
`PciFunction.open_bar(index)`/exclusive `pci_bar` lease 已实现；只有 `_CRS` 固定 consumer memory window 与 BAR base 精确匹配、
page envelope 不越界时才返回 map-only `MemoryObject`，不返回 physical address，也不提供通用 MMIO/config 写接口。
`PciFunction.read_config(offset,width)` 是独立的只读 typed scalar API，不扩展成通用 config/MMIO mapping；QEMU EDU smoke
已核对 8/16/32-bit 读取与发现到的 vendor/device ID 一致，并验证误对齐请求返回 `EINVAL`。
`DEVICE_MMIO` view 只允许 shared、non-executable mapping，`MemoryObject.read/write` IPC 被拒绝；写权限遵循 `_CRS` 的 `_RW`。
MSI/MSI-X 的 typed `route_interrupt` 与 `InterruptSource` kernel lifecycle/schema 已实现；MSI-X 配置与 kernel table mapping 已接入 route。
opt-in Q35 QEMU Rocker smoke 已端到端验证 MSI-X-only function 的 BAR `_CRS` authority、kernel table mapping、真实 vector-0 delivery、ack 与 teardown；
QEMU trace 还确认 remappable MSI-X memory write 命中所分配 IRTE。真实硬件覆盖仍未完成。`PciFunction.grant_driver`
现在验证 target Process capability、unstarted/live 状态、typed read/DMA/IRQ profile 与 6-bit BAR mask；BAR 授予必须已有匹配
function 的 `_CRS` attestation。它为 target process mint rights-attenuated `PciFunction` proxy，并返回只含
`NAOS_SERVICE_PCI_FUNCTION` one-shot entry 的有界 private ServiceDirectory；target 获得仅对此目录生效的 local-register right，父
`pcied` 保留 observer handle。`servicekit::boot::PreparedEarlyService` 已提供
deferred prepare → grant → 标准 ServiceDirectory/stdio bootstrap → Process.start 两阶段接口。仅在
`NAOS_PCIED_MSI_ROUTE_SMOKE` 测试构建中，PCIED 先将 `PlatformPciTopology` 为 QEMU EDU fixture 发布的 `QEMU0002` compatible ID
匹配到 opt-in catalog entry，再读取只对自身 scoped view 发布的独立 `edud` boot image，按 `/edud` 路径启动单独的驱动 ELF（不再把
PCIED manager ELF 复用为 child），并为两个匹配 function 各自授予 read-config、DMA-domain、interrupt-route 与 BAR0。两个 child 都验证读取 config、
映射各自 BAR0、被拒绝打开未授权 BAR1、无法 resolve discovery URI；每个 child 自己
创建/关闭 DMA domain 与 fault source、建立/ack/teardown MSI route、执行 allowlisted DMA 和未映射 IOVA fault。镜像 locator 只解析
一次并复用 immutable image bytes；父进程逐个 wait 到两个 child 都以状态 0 退出。它是两个同型 QEMU 设备的端到端
grant/bootstrap/owner smoke，不是生产 driver manager、异构设备分配策略或热插拔生命周期。该 opt-in 测试使用注入的 QEMU EDU
`_CRS` fixture 为每个 function 创建独立的 default-deny `PciDmaDomain`，每个 domain 只映射一个 DMA backing page，并有界启用/撤销 BME；
两个真实设备分别执行 DMA 写入、MSI
route、bind、raise、wait、device ack 和 kernel ack；随后还向相邻未映射 IOVA 发起 DMA 写，并要求 QEMU VT-d trace 记录匹配
requester/地址/写方向的 translation fault、已授权 backing page 保持不变。EDU 在 DMA fault 后仍会按命令位发 MSI，因此测试不把
IRQ 当作 fault verdict；独立 `PciDmaFaultSource` 必须在 BME 仍开启时变为可读并返回匹配记录。随后停止 BME、撤销映射并关闭
route/domain。该 opt-in 测试覆盖两个 QEMU EDU requester 各自的单页 allow/deny 与 MSI 事件闭环，但不代表真实 DMA 设备、生产
driver grant/binding 或通用驱动 DMA 生命周期已实现。正常
Q35 AML 未为关联 endpoint 发布符合策略的固定 consumer memory `_CRS`，所以普通 boot smoke 不会 mint BAR authority 或创建 DMA
domain。`platformd` 现将 `_PRT` 解码为有界 typed facts，并通过 generation-bound、分页的
`get_interrupt_routing` 发布 direct-GSI 或 link-device/opaque-ID 映射；畸形、重复 pin 或超限表不发布部分路由，设备标记 degraded。
这只完成 Phase 4 的路由事实输出，不代表 INTx route selection/configuration 已实现。`set_power_state`、`subscribe_changes`、
跨服务 PCI tree publication、通用 PCI config write 和 trusted-driver override 仍未实现；这些是独立的后续能力，不作为当前
`InterruptSource`/`PciDmaDomain` 合同的替代实现。G11 的 QEMU block smoke 现会注入 terminal DMA fault 并验证 child exit、catalog removal、DMA
mapping 与 MSI route teardown；kernel C++ MOVE/activate 回归覆盖两个 resource table 之间的 Domain/FaultSource 迁移和原 owner 校验。
真实硬件验证仍未完成，自动重启和 PCI hotplug 仍属非目标。
不得为上述未实现能力创建空服务。

`PciDmaDomain` 与 `PciDmaFaultSource` 在创建时固定 kernel caller PID；`PciFunction.create_dma_domain` 将经 owner 校验的 PID
传给同一对对象，domain 的 map/unmap/BME/close 与 fault source 的 take 每次都校验当前 caller PID，不匹配则在硬件操作或队列消费前
返回 `EACCES`。NaoIDL 的 `create_dma_domain` response validator 明确要求两个 owned resource 带 `NA_RIGHT_TRANSFER`，因此仍保留该
metadata right；handle move 不会重绑定对象 owner，接收方可以持有句柄但不能行使 DMA/fault authority。C++ 回归通过真实
`resource_table_t::take_native_batch` MOVE 与接收表 `activate_native` 将 Domain/FaultSource capability 移到另一张表，确认新表持有者的
DMA 操作与 fault take 均被拒绝、原 owner 仍可关闭 domain 并消费 fault。该回归覆盖 kernel resource-table transfer primitives，不是完整
Channel/NaoIDL invocation 的多进程 QEMU 测试。opt-in 双 EDU QEMU smoke 验证两个合法 child owner 仍各自完成 DMA/fault/MSI 闭环。

VT-d fault records 现在经独立、move-only 的 `PciDmaFaultSource` capability 返回给与 `PciDmaDomain` 相同的持有者。kernel 按
DMAR unit、segment 和 requester ID 将 primary fault-record registers 的有界记录分派到对应 function 的 bounded queue；fault
source 通过普通 capability readiness/epoll 通知可读，并以 `take(limit)` 返回 IOVA、requester ID、方向、reason、本队列丢弃数，
以及独立的 `unit_overflow` 标志。若 VT-d 的 unit-wide PFO 位丢失了 requester 不明的 fault，kernel 会通知该 unit 上所有存活的
fault source；该标志不伪装成精确的丢弃数量。
每个 unit 只有在 IRQ handler 已注册后才会编程 FEADDR/FEDATA/FEUADDR 并解除 FECTL.IM 屏蔽；Fault Event 是直接送往 BSP xAPIC
的 remapping-hardware event，不走 IRTE。hard IRQ 只排入软中断，软中断 drain fault ring 并向精确 requester 的 source 发布记录。
若一轮 drain 发现仍有 valid record，soft IRQ 会重新排入同一 unit；通用 soft-IRQ pass budget 耗尽时交还 daemon 续办，避免硬中断
上下文无界循环，也避免将残留记录遗留到下一次无关 fault。
若固件留下未屏蔽的 FECTL、目的 APIC/vector 不可编码、或 route 状态不确定，该 unit 不获准创建 DMA domain。QEMU EDU smoke 已验证
source readiness 与记录可在 BME 仍开启时交付；生产 driver grant、真实硬件覆盖和完整 driver 生命周期仍未完成。

QEMU EDU 通过 BAR0 的 MMIO 寄存器触发并确认中断（见 [QEMU EDU device
specification](https://www.qemu.org/docs/master/specs/edu.html)）。普通 Q35 ACPI namespace 没有关联的 per-device memory `_CRS`；
只有 opt-in smoke 注入专用 SSDT fixture 后，才可通过 kernel attestation 打开该 BAR。该 smoke 验证真实 EDU requester 的
BAR mapping/liveness、QI context-cache 与 IOTLB invalidation、default-deny DMA domain 中单页 allowlist、64 字节设备到内存
传输及前后哨兵；随后对相邻未映射 IOVA 发起写入，检查 QEMU VT-d fault trace 与 backing page 未被改变；此外还覆盖 IRTE/MSI
配置、设备 MSI 投递和两侧 ack，最后先撤销 BME，再 unmap 并关闭 DMA domain/interrupt route。这只是 QEMU EDU 的单一有界
allow/deny 用例，不替代真实设备、固件 `_CRS`、多硬件验证、系统化 IOMMU attack-surface 测试或生产 driver grant 的验收。

x86 启动路径现已为支持 QI/IR 的 DMAR unit 初始化 QI/IR table，并以 MADT IOAPIC ID 匹配 DMAR scope、沿 PCI bridge path
解析精确 source ID。驱动建立的活动 IOAPIC route 会先被 mask，再装入 source-validated IRTE 和 remappable RTE；只有所有
活动 route 均已转换且所有 unit 的 IRE 完成确认后才恢复这些 route。Q35 split-IRQ-chip boot smoke 验证了 requester-ID
解析和 IRE enable。PCI MSI capability scan、IRTE/config sequencing 和 `InterruptSource` protocol 已实现。默认 boot smoke
不建立 MSI route；仅 `NAOS_PCIED_MSI_ROUTE_SMOKE=ON` 时，额外的 Q35 EDU smoke 会使用固定 `_CRS` fixture 与 default-deny
DMA domain（仅映射一个 backing page）；EDU BAR0 发起 64 字节 DMA 写入且测试核对映射范围内的数据和相邻哨兵，然后再向未映射
IOVA 发起写入并以 `vtd_dmar_fault` trace 验证 VT-d 拒绝；在关闭 BME 前，测试还要求 `PciDmaFaultSource` readiness 被唤醒并核对
requester ID、IOVA 和写方向。EDU 即使在该 fault 后也会发 MSI，因此测试不把 MSI 当作 fault verdict。随后通过 `InterruptSource`
收到、ack 两次真实 MSI，并验证 BME 关闭、DMA unmap、route drain/IEC
teardown 与 DMA context 撤销。QI 启用后，PCI context/IOTLB invalidate 也使用 queued descriptors；不得再写 legacy invalidation
register。该 QEMU 测试不覆盖真实硬件、多平台验证、系统化攻击面或生产 driver grant。

本期必须支持 Intel VT-d 与 AMD IOMMU（AMD-Vi），但 vendor-specific register、firmware table、translation table、command/
invalidation、fault/event 与 interrupt-remapping 格式必须留在各自 backend；`PciDmaDomain`、`PciDmaFaultSource`、
`InterruptSource` 和 PCI driver protocol 只依赖 vendor-neutral kernel contract。启动代码按有效的 DMAR/IVRS hardware inventory
为每个 requester 选择唯一 backend/unit；inventory 缺失、损坏、scope 歧义、reserved-memory 语义未支持或状态未知时，DMA domain
和依赖 interrupt remapping 的 MSI route 均 fail-closed，不能把一个厂商的寄存器或表格式交给另一个 backend 解释。

AMD-Vi backend 的规范输入为 ACPI IVRS：必须有界解析 IVHD `10h`/`11h`/`40h`、IVMD `20h`–`22h`、fixed device entries
`01h`–`04h`、alias/extended/special entries 与 variable HID entries，拒绝重复/重叠或无法解析的 requester scope，并只为已证明
属于该 unit 的 PCI function 创建 domain。AMD 规范将 IVHD `10h` 定义为向后兼容格式，`11h` 与 `40h` 提供更完整的能力/设备描述；
`11h`/`40h` 的固定 header 均含 EFR 与 EFR2 register images，且仅在 IVRS `EFRSup` 置位时有效；
同一 IOMMU 有多个格式版本时，backend 必须按规范以最高支持版本作为该 unit 的权威描述，不能把兼容性副本算成第二个 unit，也
不能拼接不同版本的 device-entry list。当前 parser 按 `(PCI segment, IOMMU register base)` 识别同一 unit，并独立保存每个 IVHD 的
device-entry span；IVMD 记录关联到规范顺序中紧邻其前的 IVHD，不得跳过或重新归属。QEMU 同时
输出 `10h`/`11h` 仅作为这一规范选择规则的测试 fixture，不决定格式兼容策略。AMD DeviceID 是 segment-local 的 16-bit hardware
identity，不得假定它总等于 PCI BDF；必须遵循 alias/range/special-device 解析结果。

每个 AMD Device Table Entry (DTE) 为 32 bytes，最大 65,536-entry table 为 2 MiB；table 尺寸、对齐和连续物理范围必须按
IVHD coverage 有界计算。默认拒绝 DTE 必须为 `V=1, TV=1, GV=0, Mode=000b, IR=0, IW=0`：按 AMD 规范，Mode 0 不执行地址
translation，DMA read/write 由 IR/IW permission bits 控制，因此两类访问都被拒绝。设置 `V=1` 时 DomainID 仍必须有效，并且必须
完成 Device Table invalidation 后才能依赖新状态。全表 default-deny DTE 共用一个保留 DomainID；DMA domain allocator 必须排除该 ID，
并且不得在未完成对应 DomainID translation invalidation 前复用旧 ID。`V=0` 明确会 pass through，禁止用作未授予设备的默认项。
AMD IOMMU MMIO aperture 至少保留 16 KiB；若硬件报告 performance-counter support，规范要求 512 KiB，backend 必须先检查该能力并在
完整范围通过物理资源 ledger 后才映射寄存器。
获准 domain 必须使用独立 AMD I/O page-table v1 encoder 与有界 4 KiB 映射，并通过 bounded AMD command buffer 和 completion wait
完成 DTE/PTE publication 与 IOTLB/device-table invalidation；ATS/device-IOTLB 在其独立 invalidation contract 实现前保持关闭。
AMD controller 可 drain 16-byte event-log records 并解码 requester、IOVA、方向与 raw reason；ACPI DMA provider 以 1 ms bounded poll
将 DMA fault 投递到对应 `PciDmaFaultSource`，由普通 capability readiness/epoll 通知 owner。硬件 event-log overflow 或 drain failure
会 quarantine unit、关闭该 unit 的 fault sources 并停止相关 BME；有界 source queue 丢记录时通过 dropped count 告知 owner、停止对应
requester 的 BME 并拒绝重新启用。timer 无法续排时，关闭 fault sources 并停止所有已登记 AMD requester 的 BME。PCI MSI per-requester
route 现用 DTE `IntCtl=10b` 与 basic IRTE table；首次挂接时 DTE 保持 abort，先发布 table pointer 并完成 interrupt-table invalidation，再启用
remap 并完成 DTE invalidation。最后一条 route 撤销先将 DTE 切回 abort 并失效 DTE，再撤销 IRTE、清除 table pointer 并完成相应 invalidation。
interrupt-remapping word 按 AMD §2.2.2.2 使用单次对齐 64-bit 写。route token 绑定 requester、table slot 与非复用 cookie。AMD
controller fake-register/memory-callback 回归覆盖 PCI requester table 共享、两阶段安装/撤销 invalidation 顺序、未知 command-tail 写结果的
unit quarantine，以及 reserved `NumIntRemapSup` 只停用 MSI 而保留 default-deny。新增 IOAPIC route cases 覆盖 IVRS source 授权、
vector/table index 一致、pass-through→abort→remap 顺序、最后一条 route 退役后的 pass-through 恢复与未知 completion quarantine。
2026-10-25 controller 覆盖新增直接 DMA domain create/map/unmap/destroy、DTE/PTE/IOTLB invalidation、event-log overflow quarantine，以及
unmap/destroy invalidation 未知结果时隔离 unit 并保留页表分配的检查；destroy 失败用例也确认 PTE 仍在。AMD/通用 DMA fault-source bounded overflow 的现有回归也在本轮通过。AMD
fault-dispatch helper 现由 AMD poller 调用，测试实际 `PciDmaFaultSource` 的 requester 过滤、READABLE/take record、方向/raw reason、interrupt 排除和
队列满后的单 requester quarantine callback；unit-wide event overflow 与 drain failure 还验证只 fan-out 到所属 unit 并发送 overflow/close 状态。
AMD poller 与单测现共用 drain outcome -> controller quarantine -> requester source/BME callback 的决策 helper；新增组合回归验证
意外记录会触发 unit quarantine、关闭真实 fault source，并请求对应 requester 的 BME disable；另有 clean poll、drain failure 和 event overflow 三分支回归。controller/router/event-parser/fault-dispatch 相关 CTest 共 54/54 通过。完整 ACPI poller 到硬件 BME
停止、domain owner teardown 的组合生命周期仍未验收：QEMU 11.1.1
发出的 IOPF 记录把 memory page-walk fault 标为 `I=1`，guest 因而按规范排除该 interrupt transaction；当前 smoke 只证明 DMA deny trace，
不宣称 AMD 硬件 fault readiness 或完整生产 owner teardown 已验证。AMD interrupt remapping 使用 DTE 关联的
interrupt-remapping table 与 AMD IRTE；安装、撤销和 in-flight drain 必须执行 AMD interrupt-table invalidation。实现依照
[AMD I/O Virtualization Technology Specification, Doc 48882 Rev. 3.11](https://docs.amd.com/v/u/en-US/48882_3.11_IOMMU_PUB)；
Intel backend 继续遵循 [Intel VT-d Architecture Specification](https://cdrdv2-public.intel.com/831418/vt-directed-io-spec.pdf)。

QEMU 11.1.1 AMD-Vi model 在 Q35 以 `amd-iommu,dma-remap=on,intremap=on` 和 split kernel irqchip 启动。512 MiB boot smoke 已确认
IVRS `11h` inventory、AMD aperture admission、default-deny 和 timer-ready fault poller；EDU smoke 已确认 authorized DMA、MSI remap、
unmapped IOVA 未写入 backing page，并在 QEMU `amdvi_page_fault` trace 中看到拒绝。AMD controller fake-event test 使用规范格式验证
I/O page-fault 的 `TR`/`PR`/`RW`/`I` 解码；fault dispatch 仅在 AMD 规范允许时报告读写方向，`PR=0` 保留为 unknown，意外记录的
`TR=1` 请求 unit quarantine。AMD/通用 DMA、router 与物理资源/FACS 组合 CTest 现为 106/106 通过。2026-10-25 AMD EDU smoke 在 512 MiB 下通过；kernel log
该记录是未修补 QEMU 的历史结果；后续以 AMD-spec fixture 的正向验收为准，见下文。
`pcied` 的 denied-write smoke matcher 保留 requester/IOVA 校验，接受 direction 1（write）或 2（unknown），拒绝 0（read）；对应 feature-enabled
PCIED host suite 35/35 通过（含该 matcher regression），最新 NaOS fault-enabled Release 镜像的 AMD EDU QEMU smoke 也通过。

未应用测试夹具时，QEMU 11.1.1 的 `amdvi_page_fault()` 对 page-walk fault 无条件设置 `IOPF_I`；AMD 48882 Rev 3.11 Table 57 明确定义 `I=1` 为 interrupt
transaction、`I=0` 为 memory transaction。NaOS 按 AMD 规范将 `I=1` 事件从 DMA fault source 中排除，因此 stock QEMU 发出的 DMA fault
不会成为 guest `PciDmaFaultSource` record。未修补 QEMU 的默认 EDU smoke 只断言 DMA deny trace，不把 emulator 的错误 event flag 当作
硬件规则，也不宣称真实 AMD 硬件已通过。参考：[AMD IOMMU specification Table 57](https://docs.amd.com/api/khub/documents/GD6kOXjzWsek8QUbn_qMvg/content),
[QEMU 11.1.1 AMD-Vi event construction](https://github.com/qemu/qemu/blob/v11.1.1/hw/i386/amd_iommu.c).

测试夹具 [`qemu-11.1.1-amdvi-memory-iopf.patch`](../tests/fixtures/qemu-11.1.1-amdvi-memory-iopf.patch) 修正 QEMU 11.1.1
的 AMD IOPF 字段与跨 qword 地址掩码，不改 guest kernel。开启 `NAOS_TEST_AMDVI_MEMORY_IOPF` 并将 fixture-patched QEMU 放在
`PATH` 前部后，512 MiB `q35_pci_amd_vi_dma_msi_fault_trace_boot` 通过：event raw 为
`2070000000000018:0000000000005000`，fault source 收到 requester `0018`、write direction 和 `0x5000..0x503f`；source close
后仍成功 drain 第二批 fault；DMA domain、MSI source 关闭且 PCIED 观察到 child status 0。QEMU trace
`pci_cfg_write edu 00:03.0 @0x4 <- 0x103` 确认该 requester 的 BME 已清零。G11 的 shared backend conformance、AMD memory-IOPF
delivery、requester BME disable 与 owner teardown QEMU 核心门槛现均通过；真实硬件测试不是验收门槛。

`_OSC` 当前仅有 fail-closed 的引导期子集：`platformd` 对每个 root 只声明已实现的 `segment_groups` OS support 位
（bit 3），control mask 保持为零，执行 query/commit 并缓存协商结果；`pcied` 在 root bind 前用完全相同的 support mask
读取该 generation-bound 结果并核对 root lineage。
`PciRootFeatureSet` wire 同时保留 query/commit support、query 阶段控制 grant、最终 grant/denial/masked bits、两阶段错误、
boot/firmware/AML/topology/PCI-fabric generations 与 compatibility-profile identity。`pcied` 对照 root 校验完整 lineage，
并要求 query support 是声明 mask 的子集、commit support 与最终 query support 完全一致且两阶段无错误；
Capabilities Masked 后合法收窄的协商仍可提供只读 discovery，不完整或越界的协商才会令 topology degraded。
在线方法只返回该精确 policy-mask 缓存结果；其他 support/control request 返回 `NOT_SUPPORTED`，不会在任意
client 请求时重入 AML。未获得 `_OSC` 结果时，degraded topology 仍可用于只读 discovery，但绝不产生 native control authority。
kernel 的 G14 evidence validator 要求匹配的 owner-bound PCI_Config root scope、完整 query/commit evidence、implemented masks 与
generation/profile lineage；有效的零 control grant 验证成功且不创建 capability。`platformd` 已对每个完整 root 提交此 evidence，
Q35 smoke 验证了零 grant 下 `pcied` 继续只读 discovery。由于生产 control mask 仍为零，正向 control request 仍返回
`NOT_SUPPORTED`，也没有对应的 kernel authority object；这不阻止当前 G14 门槛验收，因为 G14 要求证明协商与拒绝路径，不要求
在没有硬件 consumer 时发明 native-control authority。G14 的 per-root query/commit、lineage 核对与零 control policy 已完成；
未来只有在引入对应硬件 owner、事件/配置职责和恢复语义后，才能扩展 mask 并实现、验证相应操作。G13 的 `PciFabricAdmin`
仍只是独立的只读 ECAM discovery capability，不能充当 native feature ownership。

`_OSC` control DWORD 是按规范定义的 feature ownership 协商，不是可由调用者请求的通用 “native PCI” 开关；任何正向 bit 都必须
先有对应的硬件操作 owner、事件/配置职责、恢复语义和后续协商保留规则。当前没有这样的 root-wide native control consumer，
因此 kernel 与 platformd 的生产 control mask 都保持为零。evidence validator 的 synthetic positive fixture 只测试证据一致性，
不表示生产支持；独立负向测试锁定生产策略对任意非零 control request 都 fail-closed。[ACPI 6.6 §6.2.12](https://uefi.org/specs/ACPI/6.6/06_Device_Configuration.html)

当前 `platformd` 会为已置位的 GPE status 解析唯一的 `\\_GPE._Exx`/`\\_GPE._Lxx` AML handler，并通过 dedicated AML worker
执行；edge status 在执行前清除，level status 在对应 handler 返回后清除，时序遵循 [ACPI 6.6 §5.6.4](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html)。
清除只能经 owner、generation、SCI sequence 与本次 drain 观测位共同约束的 `AcpiFixedHardware.clear_sci_status` typed RPC；
未知/歧义 handler、AML 失败或不支持的 PM1 fixed-event status 都保持未处理并令 SCI source fail-stop 留在 masked 状态。PM1 timer carry
status bit 0 (`TMR_STS`) 是唯一无 AML consumer 的安全确认项；其 `TMR_EN` 已清零，OS 不使用 PM timer interrupt 时只清该 observed
status，不执行 AML，符合 [ACPI 6.6 §4.8.1](https://uefi.org/specs/ACPI/6.6/04_ACPI_Hardware_Specification.html)。
其他 PM1 fixed events 尚无 consumer。kernel 在 SCI source 启动、claim 以及 owner 失效/报告失败时清零 PM1/GPE enable；PM1 enable
区分为 status block 的第二半，不能写 status/W1C 半区。platformd 在同一有界 SCI work unit 中扫描固定 GPE 范围，只通过 owner-bound
`set_gpe_mask` 启用唯一 AML handler 对应的位。Q35 CPU-hotplug smoke 将 `maxcpus=3`，通过 QEMU monitor 插入第三个 CPU
（QEMU 将 CPU hotplug event 路由到 GPE.2，见[接口规范](https://www.qemu.org/docs/master/specs/acpi_cpu_hotplug.html)），
观察到 GPE.2 status、PM1 TMR_STS 清除、测试 SSDT `_E02` handler 执行和匹配 sequence ack/resignal 状态 0；该 fixture 用 NoOp 覆盖默认
`_E02`，只验 SCI/GPE 传递与生命周期，不验 CPU hotplug policy。默认 QEMU hotplug AML 仍依赖未支持的 `Notify` 路径并 fail-closed，
真实硬件 GPE/EC 与热插拔仍未验收。AML deadline 或平台发现健康位仍阻止 SCI owner claim。
2026-10-24 历史验证：`q35_acpi_sci_gpe_hotplug_boot` 在 2048 MiB headless Q35、`maxcpus=3` 下通过，日志含 GPE.2 sequence、PM1 timer
status clear、`_E02` dispatch 与 `ack state=0`；C++ fixed-hardware 回归 247 assertions/25 cases、platformd host tests 58/58、Rust workspace
和 pinned NaOS Release 构建通过。2026-10-25 按正式 512 MiB 配置重跑同一 GPE gate 通过，serial 确认 handler dispatch 与
`ack sequence=1 state=0`。该 event fixture 不提供设备 Notify consumer，因此不改变 PRD 对 ACPI hotplug 未实现的判断。

FirmwareRoot 的 `list_objects` 每次最多 64 条只是分页大小；platformd 的完整 catalog/order validator 按 kernel 的 1024 表上限
接受 SSDT ordinal，不把单页大小误当成 SSDT 总数限制。

本 PRD 不试图验证 BIOS/UEFI firmware 的善意、签名或电气安全，也不让 AML 可访问 normal RAM、kernel text/data、
任意 MemoryObject 或用户进程地址空间。

## 4. 组件、信任域与启动顺序

| 组件 | 可信职责 | 可持有的根能力 | 明确禁止 |
| --- | --- | --- | --- |
| kernel | table snapshot、早期启动、VM/IPC/capability、硬件机制 | platform hardware roots | 完整 AML namespace 或设备策略 |
| `platformd` ACPI backend | Rust `acpi` interpreter、namespace、AML 求值、平台策略/快照/API | `FirmwareRoot`、`AcpiResourceAuthority`、其私有的 firmware PCI-config scope | transfer/duplicate 硬件根能力、向外提供 raw AML shell、持有 `PciFabricAdmin` |
| `platformd` DT backend（后续） | Device Tree 解析、平台策略/快照/API | `FirmwareRoot` | 引入 AML/ACPI-specific public ABI 或持有 `PciFabricAdmin` |
| `pcied`（本期） | 读取 platform root facts 与 compatible identifiers、执行 MCFG intersection、只读 ECAM 扫描及进程内 PCI tree 构造；可请求 typed MSI route | `PciDiscoveryFabric`；按 intersection mint 的 owner-bound `PciFabricAdmin` 与 `PciFunction` capability | 正常路径向 driver transfer capability、超出 bound root 访问或直接执行通用 PCI config/BAR/IRQ/MSI 写操作；仅 opt-in EDU catalog entry 执行受限 child grant |
| `pcied`（后续） | host-bridge/resource policy、driver binding 与 rights attenuation | 后续 bound `PciFabricAdmin` 和已审定的单 function capability | 维护或调用 AML namespace、让 driver 枚举全 fabric |
| driver | 操作一个已分配 PCI function | attenuated `PciFunction` | 任意 physical/MMIO/config access |

同一 `platformd` process 的 capability table 不能隔离其 ACPI executor 与 policy module；这是有意接受的 shared-state trade-off。
但 `PciFabricAdmin`、root-wide ECAM mapping、BAR assignment、bridge configuration 和 IRQ delegation 留在 `pcied`，
所以畸形 AML 或 ACPI policy compromise 不会自动成为全 PCI fabric 的重配置权限。端口 lease 仍应绑定 AML execution
thread，以减少 `platformd` ACPI backend 内的意外直接 I/O。

若未来需要把 firmware interpreter 与电源策略隔离，可再将 ACPI executor 从 `platformd` 拆开，并将内部 `AcpiExecutor` contract
升格为 NaoIDL；这不是第一阶段的必要复杂度。`pcied` 已经是独立 process，任何后续拆分不得把其 `PciFabricAdmin` 变成
可被 `platformd` resolve 的公共资源。

bootstrap 仍只包含通用 ServiceDirectory、stdio 和 process-start contract。内核资源不得变成服务专属 bootstrap
capability；kernel 必须先在各服务受 scope 限制的 ServiceDirectory namespace 中发布资源，再由服务 resolve。示例：

```text
platformd -> naos://kernel/firmware/root
          -> naos://kernel/hw/acpi-resource-authority              # ACPI backend only
pcied     -> naos://kernel/hw/pci-discovery
```

这些 URI 不得在任意进程都能 resolve 的全局 namespace 中无条件暴露。目录 view 本身是 capability；service manager
必须给 `platformd` 与 `pcied` 各自只包含所需入口的 view。派生 view 必须强持有其 parent、只接受 parent allowlist 的精确 URI
子集，且不得扩展 resolve 或 publish 权限。以可猜 URI、PID、可执行文件名或 AML 路径作为授权依据均不成立。

当前 `ServiceDirectory.create_view(uri)` 只接受显式 `CREATE_VIEW` protocol right，且只为 parent 已允许的单个精确 URI
派生只读 view；返回 view 强持有 parent、仅保留 `INVOKE` protocol right，不能继续创建子 view。对 parent 范围内但不在该
child URI 中的 locator，resolve 仍返回 `EACCES`。opt-in QEMU EDU smoke 覆盖成功 resolve、nested-view 拒绝和相邻 locator
拒绝。这个通用 directory attenuation API 不创建 PCI function capability、不选择 driver、也不构成 production driver grant。

`PciFunction.grant_driver` 使用独立、未注册到全局目录的有界 ServiceDirectory：kernel 预置一个 move-only
`PciFunction` entry，URI 固定为 `NAOS_SERVICE_PCI_FUNCTION`；target 获得只作用于该私有 registry 的 local-register right，可向
父 `pcied` 发布额外 endpoint。registry 最多保留 8 个 entry/listener，包含预置 function entry。双 EDU smoke 中两个 child 各自发布
一个 MemoryObject probe，父 manager 经 observer handle 读回数据，并确认相同 URI 在全局 `pcied` directory 中不可解析。该 function proxy 的 owner 是请求中 Process capability 指向的 target，profile 将 typed method rights
和 BAR index 再次收窄。它不是从 `pcied` 的普通 discovery view 派生的子集，因此不把
`PciDiscoveryFabric`、其他 PCI function 或 global ServiceDirectory 一并暴露。正常启动的固定 Virtio Block loader 与 opt-in EDU
smoke 都用 `PreparedEarlyService` 在 grant 完成后将私有目录作为标准 bootstrap ServiceDirectory 安装到未启动 child。EDU smoke
验证授权 config/BAR0、未授权 BAR1、相邻 locator 拒绝和未映射 IOVA fault；Virtio Block QEMU smoke 验证 child 使用该 capability
执行读写。PCIED 目前只为固定 Virtio Block binding 和 opt-in EDU 管理 child，不是通用 driver manager。
被丢弃的 deferred child 在最后一个 Process capability 关闭时由 kernel abort/reap，不会因 grant/bootstrap 失败遗留未启动进程。

opt-in child smoke 所需的独立 `edud` ELF 作为可选 Multiboot 模块，仅在 smoke 构建中打包，并由 kernel 作为只读
`MemoryObject` 发布到固定 boot-module URI、只加入 PCIED 自身的 scoped ServiceDirectory view。该镜像 locator 不在 grant 后 child
的私有目录中，也不暴露通用 boot-module namespace。driver local-register right 只写入 grant 创建的私有 directory，不可用于该 boot-module view 或全局 registry。

启动顺序分为无循环的三个阶段：

1. kernel 发布 `FirmwareRoot`、ACPI executor（仅 ACPI platformd scope）与 MCFG-backed `PciDiscoveryFabric`；`platformd`
   的 ACPI backend 加载 tables/namespace 并发布带 generation 的 `PlatformPciTopology` root facts。
2. `pcied` resolve 两个受 scope 限制的 service，计算每个 root 与 MCFG allocation 的交集，调用 `bind_root`；kernel 校验
   epoch/firmware/fabric generation、segment、MCFG containment 后 mint 当前 process 专属的 `PciFabricAdmin`。该 capability
   只能打开其固定 bus window 的 read-only ECAM，并为扫描到的 BDF mint `PciFunction` identity capability。
3. 正常启动只对精确匹配的 Virtio Block function 执行 deferred child grant/bootstrap；`virtioblkd` 获得只读 config、attested BAR、
   `route_interrupt` 和 IOMMU DMA-domain authority，不获得任意 PCI config/BAR 写权限。Opt-in QEMU smoke 已验证单队列 MSI-X
   write/flush/read-back 和 VT-d interrupt-remapping。typed `PciFunction.route_interrupt` 也由 EDU/Rocker smoke 验证 conventional MSI
   与 MSI-X。真实硬件 DMA、AMD IOMMU、异构 driver binding 与故障/热插拔 lifecycle 尚未完成。
   完整 host-bridge resource policy、`_OSC` 正向 ownership 消费和无 IOMMU trusted-driver policy 仍未完成。

当后续启用 AML `PCI_Config` OperationRegion 时，firmware PCI-config scope 的 grant 才是低频 `platformd ↔ kernel`
控制面；本期 `pcied` 不参与该路径。该阶段 token 只取得 read-only PCI config mapping；任何 AML PCI-config write 返回
`NOT_SUPPORTED_PCI_CONFIG_WRITE` 并使受影响平台 feature degraded。任何将来 write 的冲突仲裁均属于后续高风险阶段，不能借由
把 `PciFabricAdmin` 转交给 `platformd` 来规避。

## 5. `FirmwareRoot` snapshot 合同

kernel 从 bootloader 提供的 firmware root 开始建立 immutable snapshot。ACPI 路径验证 RSDP、RSDT/XSDT、被引用 table 与
FADT 间接引用 DSDT 的地址、结构长度、算术溢出和规定 checksum；Device Tree 路径验证 DTB header、total size、reserve map
和 structure/string block 边界。用户态永远不能获得这些对象的物理地址、直接物理映射或可写 view。FACS 依第 2.2 节的
专门状态合同处理，不因 catalog 存在而自动获得可写 mapping。

`FirmwareRoot` 提供以下逻辑接口：

| 方法 | 调用者 | 结果与限制 |
| --- | --- | --- |
| `describe()` | `platformd` | `kind`、generation、format revision；不返回物理地址 |
| `list_objects(cursor, limit)` | `platformd` | `FirmwareObjectDescriptor[]`；ACPI descriptor 可含 signature/instance/revision，DT descriptor 可含 blob/overlay type |
| `open_object(object_id)` | `platformd` | 只读、固定长度 `MemoryObject`；返回对象的 `WRITE`、resize 和 executable 权限永远不存在 |
| `list_relations(cursor, limit)` | `platformd` | `FirmwareRelation[]`；只返回 opaque object IDs、relation kind 与 ordinal，不返回 physical address |

提议的 admission 上限为：单 table 1 MiB、catalog 总量 16 MiB、table count 1024、每页请求至多 64 descriptors；
具体数值须成为 kernel ABI 配置常量并在 QEMU/真实固件样本上复核。超限 table 不得部分暴露，catalog 应标记为失败。
FACS 等不适用普通 checksum 规则的 table 必须在 descriptor 中明确标记；不可因为兼容性而跳过长度和物理范围检查。

table snapshot 在一次启动中 immutable。动态 table install、OS override 与 table hot-replace 不属于第一阶段；若日后支持，
必须由内核生成新的 `firmware_version` 字符串、使用新 namespace 并执行显式重初始化事务，而不是原地修改映射字节。

`FirmwareRelation` 是安全 table-reference graph，而不是让用户态重新把表中裸地址解释成 mapping request。ACPI 至少发布：
RSDT/XSDT 的有序 child edges、FADT -> DSDT edge，以及 FADT -> FACS-state edge；每个 root edge 带 `root_ordinal`，
每个 AML table 带确定的 `aml_load_ordinal`。`platformd` 的 Rust ACPI adapter 必须按该 graph 加载 DSDT 后再按
`aml_load_ordinal` 加载 SSDT，不能按 `{signature, instance}` 排序或按 FADT 内的数值地址再次 lookup。FACS relation 只描述
其存在；第一阶段不产生可写 FACS mapping。

## 6. 硬件 capability 合同

内核不提供 `read_physical(address, size)`、`write_port(port, value)` 或“任意 BDF config read”这类通用接口。每种对象
具有可信 resource-table entry、binding、scope、generation 与 protocol rights。

| capability | 范围 | 可执行操作 |
| --- | --- | --- |
| `MmioRegion` | 固定 physical range、cache type、R/W map rights | 向指定 address space 建立有界 user mapping；随后 CPL3 volatile load/store |
| `IoPortLease` | 固定 16-bit port range、绑定到一个执行线程 | CPU-enforced CPL3 直接 8/16/32-bit `in/out`；每个 port 的硬件权限同时允许两种方向 |
| `PciConfigSpace` | 固定 segment + root-bus window 或单一 function、explicit read/write map rights | 仅 ECAM 的受限 config mapping；discovery PTE 必为 read-only |
| `PciDiscoveryFabric` | kernel MCFG allocation inventory | 列出 MCFG、按当前 snapshot 与 MCFG containment mint root-scoped `PciFabricAdmin`；本身不映射 ECAM |
| `PciFabricAdmin` | 单一 platform-root/segment/bus intersection、owner PID 与 generation lineage | 只读映射该 root ECAM、为 root 内已扫描 BDF mint owner-bound `PciFunction`；无 transfer/config-write/BAR/IRQ 权限，不直接授予 DMA |
| `PciFunction`（本期） | 一个 BDF、root ID 与 epoch/generations | 身份查询、绑定 kernel-minted device `_CRS` evidence、按 BAR index 打开受限 MMIO view、typed 创建独立 `PciDmaDomain`、owner-only mint `InterruptSource`；不暴露通用 config-space、任意 physical address 或裸 MSI message |
| `InterruptSource` | 一个已路由 interrupt | bind、wait、mask、ack，分别以 rights 控制 |
| `AcpiSciSource` | 平台 SCI/GPE 事件源 | 仅 `platformd` ACPI executor 订阅；内核 ISR 只投递事件 |
| `AcpiFixedHardware` | FADT fixed register groups、SCI/reset/sleep control | typed event drain/clear、GPE control、sealed sleep/reset transition；不是普通 region map |

每次 region/ECAM grant 和 map request 都必须校验 base/length 的溢出、完整授权区间、cache type、map protections、
目标 address-space、capability binding/scope/generation 与 rights。成功后，CPU page table 或 TSS bitmap 执行数据面
访问；不能宣称每次 direct load/store 仍会经过 kernel 的 operation-level 校验。撤销前必须停止执行者、撤销对应
mapping/bitmap 并完成 TLB/CPU 同步，防止 stale mapping。resource descriptor 只有在发送方拥有 `TRANSFER` 时才可移动；
接收方只能得到相同或更弱的 rights。

所有 physical hardware capability 都必须从 kernel-owned `PhysicalResourceRoot` 派生为 `ResourceLease`，并登记到全系统
resource ownership ledger。ledger 记录 parent、物理 byte range、`page_cover`、resource kind、cache/protection、owner、
sharing policy、generation 与 revoke state；kernel reservation、ACPI fixed hardware、AML SystemMemory、PCI BAR、ECAM、
driver MMIO/DMA window 均使用同一 ledger。lease 状态机至少为 `reserved -> leased -> mapped -> revoking -> revoked`，所有
grant/map/revoke 在同一 ledger transaction 中线性化。`pcied` 只能从已绑定的 parent root 派生 BAR child lease，不能因看到
物理地址自行 mint `MmioRegion`。Top-level reservation 必须传入绑定该 ledger 的 kernel `PhysicalResourceRoot` 对象；
数值 root sentinel 只用于 lease parent lineage，不构成 reservation authority。
当前 kernel ledger 已拒绝 sentinel-only top-level reservation；ACPI、ECAM、FACS、fixed-register 与 DMAR aperture 的初始 lease
均经实际 root authority admission。
DMA domain 现在也通过同一 `PhysicalResourceRoot` 为每个 backing frame 登记 kernel-owned、exclusive、WB `dma_window`
lease；IOMMU confirmed map failure 会回滚 lease，confirmed unmap 后才 revoke。未知 map/unmap 或 teardown outcome 会保留该 lease
和对应 `MemoryObject` backing，避免物理页在硬件仍可能访问时重新进入可用范围。DMA window 与其它硬件 lease 共用 bounded
physical ledger（512 项，由 kernel virtual allocator 承载）。2026-10-25 DMA CTest filter 48/48 通过，实际执行了
`pci_dma_domain_test` 的成功、失败、未知结果及 ownership 路径。

`MmioRegion` cache type 由内核确定。用户态不得选择或改变 PAT/MTRR 属性；页对齐扩大、与既有 mapping 的属性冲突，
以及 normal RAM、kernel memory、table snapshot 与受保护 firmware 区的请求都必须拒绝，不能静默放宽范围。
内核物理 linear map 将 allocator RAM 保持为 WB，并将其它映射页及 memory-map gaps 的 page-cover 设为 UC；必要时拆分
1 GiB/2 MiB leaf。BSP 与 AP 在切换到该页表前各自执行 cache write-back/invalidate，确保 kernel `pa2va` 与 direct user mapping
的有效 cache type 一致。该路径已编译，启动期 cache/TLB 行为仍需 boot 验证。

PTE 是页粒度，OperationRegion 是 byte 粒度；因此“PTE 不超出获授 byte range”并不可实现。direct map 仅在
`page_cover(region) = [align_down(base), align_up(base + length))` 的**所有完整页**都属于同一可授予
`PhysicalResourceRoot`、没有与 RAM/kernel/其他 device/其他 authority overlap，且 page-cover 的 cache/protection 完全一致时
才允许。页粒度会让映射比请求的 byte range 更宽；超出的页内字节由 AML token 的 logical bounds 限制。只要任一完整页
不能安全授予，kernel 就拒绝整个 region，不提供 scalar fallback。例如跨页的 `0xFED00FFC + 8` 只有在两个完整页
都满足上述条件时才可授权。

即使 direct map 获准，`RegionAccessToken` 仍必须在每次 logical access 验证 `offset + width <= region.length`，以
`mapped_va + (region.base - page_cover.base) + offset` 计算地址；不得因 page envelope 存在而允许相邻 register。ledger 要
记录整个 envelope 并拒绝有冲突的 cache alias 或并发 revoke。FACS 的完整 page-cover 分类为 `FirmwareStateReserved`，第一
阶段对 generic `SystemMemory` mapping 一律拒绝。

`MmioRegion` 只可 map 至 capability 所绑定的 address space，且 map 权限按 capability 衰减为只读或读写。它不得作为可
transfer 的 `MemoryObject` 或向外暴露为通用 shared-memory buffer。direct mapping 一旦进入 `platformd` address space，
该 process 内任何 memory-unsafe 代码都可访问它；adapter 只控制 admission，不构成 module 级内存隔离。若需这一层隔离，
必须把 AML backend 拆为独立 process。

### 6.1 x86 SystemIO：thread-bound TSS port lease

`SystemIO` region 由 `AcpiResourceAuthority` 做一次控制面 admission，然后直接在绑定的 AML execution thread 上安装
覆盖精确 port range 的 TSS I/O-permission bitmap lease。每个 port 的 x86 权限同时允许 `in` 和 `out`，不会按访问方向或
width 再向 kernel 发 RPC。逻辑范围与 width 仍由 `RegionAccessToken` 检查；当前 `platformd` 的只读 profile 也继续拒绝
AML 写操作。kernel 对 SystemIO 的 ledger access 标记为 `ACCESS`，不伪装成独立 `READ`/`WRITE` hardware rights。

授权前，`platformd` 必须在 `_REG` native-region activation 之前，对有界设备集预检 `_CRS`，只接受固定基址、无 translation
offset、16-bit 内且属于同一 AML parent 的完整 consumer I/O 范围；可变基址、解析失败、缺失或越界的描述符不产生授权。
小型 I/O Port descriptor 仅在 min=max 时归一化；AddressSpace I/O 则要求 revision 1 Extended descriptor、consumer bit、min/max
均固定、区间长度与端点一致，且 type-specific flags 表示无 translation 的 full-range decode。WORD/DWORD AddressSpace
descriptor 的 bit 0 不定义 consumer/producer，不能据此产生 SystemIO 授权。
未完成该预检或未安装 allowlist 时，所有 AML SystemIO region 都拒绝。该检查是针对 AML region 的额外约束，kernel 仍独立执行
reserved-range、owner、generation、thread、rights 与 access-width 校验；它不把 `_CRS` 声称视为硬件真实性证明。

实现合同：

- EFLAGS.IOPL 始终低于 CPL3；严禁通过提高 IOPL、`ioperm` 风格的全局开关或共享全 port-space capability 实现。
- bitmap 默认全 `1`（deny）。在没有 direct-I/O lease 的 thread 上，TSS I/O-map base 必须位于 TSS descriptor limit
  之后，从而使所有 user I/O 产生 `#GP`。
- 每个 CPU 使用自己的 TSS 与 bitmap；调度、CPU migration、direct lease grant、direct lease revoke 和退出用户态前必须安装正确 bitmap。
  更新时先清除旧允许位再置新允许位，并与 thread 迁移/抢占同步，不能让另一 process 继承前一 process 的 port grant。
- bitmap 每个 port 只有 allow/deny，不能区分 `in` 与 `out`、access width、值、频率或审计事件。因此 direct `IoPortLease`
  的硬件权限是 `ACCESS`，不是虚假的 `READ`/`WRITE` rights。AML 逻辑写策略由 interpreter token 在用户态执行；需要对不可信
  代码强制只读、值过滤、审计或限流时，direct I/O 不适用。
- CPU 对多字和 string I/O 会检查所涉及的 port 位；lease 的 range 必须覆盖完整访问。禁止把越界的相邻 port 当作可访问。
- 拒绝的 `#GP` 必须按用户态 access fault 处理，不能 panic kernel；不得靠捕获 `#GP`、解码指令并代为 I/O 来模拟常规路径。

当前实现使用每 CPU `TSS + 8192-byte bitmap + terminating 0xff byte`；无 direct lease 时将 I/O-map base 设在 descriptor
limit 之后，调度及迁移路径按 execution-thread lease-set 安装对应 bitmap。`tss_io_bitmap_test` 覆盖 deny-all、边界 port
和 lease 安装/撤销。bitmap 更新可按 lease-set identity 缓存或按 range delta 更新，但 revoke 必须先完成，不能改变切换原子性。

AML 的 `SystemIO` region 创建仍是一次控制面操作：ACPI interpreter 先通过同 parent `_CRS` allowlist containment，再向
`AcpiResourceAuthority` 请求验证 `(base, length)`；成功后 exact-range lease 附着到 AML execution thread 并允许该 thread
执行 OSL `in/out`。`platformd` 的 RPC、日志和表解析线程不获得 direct lease。

## 7. AML OperationRegion 与 IRQ

ACPI executor 采用 `rust-acpi-naos` security-boundary fork。上游裸 `Handler::read_*`/`write_*`/`read_io_*`/`read_pci_*`
只接收地址且不表达 capability/error ownership，不能作为 NaOS 授权点。fork 必须在 interpreter 持有完整
`OpRegion { space, base, length, parent_device_path }` 的 native-region dispatch 层授权，抽象为：

```text
RegionAccessToken = authorize(&OpRegion, access_mode) -> Result<OpaqueToken>
read(token, offset, width) -> Result<u64>
write(token, offset, width, value) -> Result<()>
```

Token 是 fork 与 native-region handler 之间的进程内 opaque value，不可序列化或 transfer；绑定 namespace generation、OperationRegion
byte range、parent device 和 direct mapping/port lease。interpreter 在每次访问前先取得 token，再将 token 与
offset/width 交给 handler；handler 按 token 检查 offset/width/overflow。底层裸 Handler 只作为 token 已建立后的实现细节，不能自己从裸地址
申请权限。每次 token authorization、native region access 和 method invocation 都向 AML fuel 计数。

PRD 合入前必须锁定 `rust-acpi-naos` commit，并证明它替换了 upstream 对 `SystemMemory`、`SystemIO`、`PciConfig` 三种
built-in region 的全部 native read/write dispatch；任何 dispatch 不得 fallback 到 raw `Handler`。token cache 的 key 至少包含
space、base、length、parent device、namespace generation、access mode 与 resource generation；dynamic region、alias、`_REG`
activate/deactivate、lease revoke 和 read-only PCI discovery phase 均会失效/重建 token。

2026-10-25 fork 基线迁移：`.gitmodules` 与 submodule `origin` 指向 `https://github.com/kadds/acpi.git`；submodule 工作树现基于
fork `main` 的 `ad7fb092596eb0fd1c89400d256ec5c9363c0406`（`v7.0.0-pre1`），父 `Cargo.lock` 也已更新到该版本。已将 NaOS 的
`NativeRegionHandler`、region lifecycle、execution meter 与 allocator accounting 改动移植到 v7 API；同时采用 v7 的 AML `Match`、
recursion/loop guards 与 `PhysicalMapping` API，并把依赖的 `RawPhysicalMapping` 测试构造改为安全边界明确的构造函数。当前 nightly
要求把上游 `allocator_ext` feature gate 写为 `allocator_api`。fork workspace 的 `cargo check --workspace --tests --offline` 与父仓库
`cargo check --offline -p rust-acpi-naos --tests` 均通过；这些命令只编译测试目标，没有执行测试。

G2 仍未达到可复现门槛：v7 基线本身可从 fork 获取，但 NaOS 移植改动尚未进入 fork 的远端 commit，父仓库 gitlink 也尚未更新为
包含这些改动的 revision。当前工作树已将 fork dispatch 收敛为 `authorize(&OpRegion, access_mode) -> OpaqueToken`，再执行
token-relative read/write；dynamic region 重定义在 namespace 替换前失效旧 token，deactivation 在 `_REG(..., 0)` 前断开 registration
并失效 token。回归覆盖三种 built-in region 无 raw Handler fallback、动态重定义失效、`_REG(0)` 不能访问断连 region、撤销后拒绝
旧 token。AML malformed-input 防护另修复 QWordField bit-offset overflow、BufferField 越界写、短 NameSeg 错误路径越界、reference-cycle
解包 hang、VarPackage heap exhaustion 和 `_ELSE` PkgLength underflow；`Match` 的越界 start index 返回 no-match sentinel。fork workspace
测试、NaOS `rust-acpi-naos` host 测试和 QEMU 门槛均通过。由于用户要求暂不提交，fork 新 revision 与父 gitlink pin 仍待后续提交。

区域数据面使用如下路径：

```text
SystemIO:     AML region creation -> exact-range thread-bound TSS lease -> CPL3 in/out
SystemMemory: AML region creation -> safe page-cover admission -> bounded CPL3 MMIO mapping -> load/store
PCI_Config:   config scope admission -> ECAM mapping cache -> bounded CPL3 load/store
```

除 `SystemIO` 的 thread-bound lease 外，`region_token` 是可信的内部 capability reference，而不是 `platformd` 或外部 caller
可以构造的 `(space, address, length)`。定义 region 时，AML 计算得到的 base/length 仍可能是动态的；内核须验证无溢出后按
address space 采用以下策略：

- `SystemMemory`：仅设备 MMIO 或 ACPI NVS/firmware-reserved 范围；拒绝 normal RAM、kernel image、用户页和 tables。
- `SystemIO`：必须完整包含于同 parent `_CRS` 的固定 consumer I/O range，且为合法 16-bit port 区间；宽度限定为 8/16/32 bit；
  direct-I/O lease 授予整个 port range 的 `in/out` 硬件权限，软件 access token 仍按 profile 限制逻辑写入。
- `PCI_Config`：仅已验证的 MCFG/root bridge segment-bus 范围；offset 限制在 config space 内。
- 其他 address space：第一阶段显式 `NOT_SUPPORTED`，不得回退为 raw memory access。

FADT 描述的 PM1、GPE、reset、sleep control 等 fixed hardware 不得借普通 `MmioRegion`/`IoPortLease` 交给 AML。它们由
`AcpiFixedHardware` 独占；ACPI executor 只通过 typed event drain/clear、GPE enable/disable、prepare-sleep 和 sealed
`commit_sleep`/`reset` 请求参与。最终 S5 sleep-enable/reset write 仍由 kernel 完成。

kernel 在接管前建立 `AcpiFixedHardware` inventory：PM1 event/control、PM2 control、GPE status/enable、SMI command、
reset register、sleep control/status，以及 kernel-private FACS Global Lock word。FACS 和 fixed-register blocks 均不经 generic
region admission 暴露；Global Lock 的并发、超时、SCI wake、release event failure 与 owner/thread exit 必须有明确且可测试的结果，
不能用重叠 lease 实现“共享”。

AML fork 在 table/namespace load 时记录静态 `LockRule=Lock`、`\_GL_` 与 `_GLK` 依赖；执行时必须处理动态 alias。显式
`Acquire/Release`、`_GLK=1` 设备方法和 locked FieldUnit 统一走 Global Lock host callback；`_GLK` 设备的 `_STA/_INI/_HID/_CID/
_UID/_CRS/_PRT` 发现不得因存在锁而整类跳过。依赖被识别本身不将平台标为 degraded；实际 acquire timeout、缺失后端、SCI
failure 或释放结果不确定必须落到对应设备的 degraded/error outcome，不能静默忽略锁语义。

`AcpiResourceAuthority` 仅可在 `platformd` 的 ACPI-scoped directory 中 resolve，且没有 `DUPLICATE`、`TRANSFER` 或向普通服务 export 的 rights。该约束将
firmware AML 作为一个显式高权限域，而非错误地把它当作不可信脚本沙箱。`SystemMemory` mapping 的撤销必须使目标
AML execution thread 停止并完成 TLB shootdown；不能仅从 capability table 删除 handle 就认为物理映射已经消失。

`HardwareSideEffectProfile` 在 native-region adapter 内、调用 kernel lease backend 之前执行 address-space 与读写 mode gate；executor
提供 profile 上限，单次 `snapshot_devices` policy 只能进一步收窄。当前受控 profile 允许 `SystemMemory`/`SystemIO` read、拒绝
`PCI_Config` 与所有 writes；`SystemIO` backend 还必须验证同一 parent 的固定 `_CRS` containment。这个 profile 不表示 AML method
或硬件 read 没有副作用（例如 read-to-clear），所有 `_STA/_CRS/_PRT` 仍按有副作用的串行 control method 处理。

Global Lock dependency 是 capability requirement，不是平台 degraded 状态。`BaseInterpreter` 将 explicit `_GL_` acquire/release
和 locked FieldUnit 委托给 host callback；设备 `_GLK` 非零时，device-scope control methods 在相同锁内执行，含 `_INI`、identity
和 `_CRS/_PRT`。动态 alias 在 opcode dispatch 时按同一 Global Lock object 解析。平台仍继续发现 HID/CID/UID 与 resources；仅当
kernel Global Lock backend 实际失败时，受影响的设备结果才标记 degraded。FACS 是 mutable state：FirmwareRoot 不开放其字节，kernel
在发布 executor 前把 FACS logical range 与完整 page envelope 记入 `firmware_state` ledger；冲突或保留失败时不发布
FirmwareRoot/executor authority。FADT 的 FACS 地址即使 alias 到已 catalog 的其它表，也会重新验证 signature/length 并拒绝错误
关系。`platformd` 不映射 FACS；kernel 只原子访问 GlobalLock DWORD 并管理 GBL event bits，typed IPC 不包含物理地址或 writable
mapping。

### 7.1 SCI mask、drain、ack、rearm 状态机

SCI 是 level-sensitive，不能实现为“kernel 置 event bit 后无限投递”。`AcpiSciSource` 使用如下状态机：

```text
unmasked SCI
  -> ISR issues required controller EOI, masks/suppresses source and atomically increments seq
  -> coalesced Event { seq, overflow/lost flag }
  -> platformd drains PM1/GPE status through AcpiFixedHardware, clears handled status
  -> ack_and_rearm(seq) validates latest seq, rechecks pending state, then resignal-or-unmasks
```

同一 mask interval 内的中断必须 coalesce；kernel 维护 pending 与 lost/overflow 标志。过期 `seq` 的 ack 不得重新启用 source。
`claim_sci` 必须绑定 kernel 提供的 caller PID；后续 drain、ack/rearm 和 GPE mask 只允许该 SCI owner。固定寄存器电源
transition 使用独立的 `claim_transition_owner` owner，不能通过认领 SCI 隐式取得；同一 `platformd` caller 可分别认领两者。
SCI owner 进程退出时，kernel 串行化在途 SCI transaction 后 mask SCI、撤销 event owner 并 sticky-degrade；transition owner
退出则另行撤销 transition ticket。任一 owner 在同一 boot 内都不得由重启进程自动接管或重放 AML。`platformd` crash、超时或
反复 rearm failure 时 SCI 保持 masked，并发布 degraded/overflow 诊断而不是制造 interrupt storm。

SCI readiness wait、drain 或 rearm 失败而 `platformd` 进程仍可运行时，必须先通过 `AcpiFixedHardware.report_sci_failure`
显式报告；请求携带 boot/firmware generation，kernel 仅接受当前 SCI owner，并在串行化 transaction 中屏蔽 source、撤销
owner、sticky-degrade 后返回状态。`platformd` 只有在确认 kernel 已进入 degraded state 后才发布新的 degraded topology generation
并停用本地 consumer。若报告本身失败，服务退出并由 kernel 的 owner-exit 路径 fail-closed。该报告不触发 AML 重放或 SCI rearm。
不得在 hard IRQ context 执行 AML，也不得允许 AML 注册任意 GSI 的用户态函数指针。

## 8. 本期边界与后续协议预留

本期已有 `PlatformPciTopology` generation/root client，以及 kernel `PciDiscoveryFabric -> PciFabricAdmin -> PciFunction`
capability 链的 NaoIDL schema。`pcied` 不发布对外 PCI topology service；它把扫描结果与 function capabilities 保存在本进程。
`PciFunction` 提供身份查询、owner-bound `create_dma_domain` 和 typed `route_interrupt`；route method 只 mint 独立
`InterruptSource`，不暴露 config register、BAR、裸 APIC message 或通用 MMIO。
创建 domain 时 kernel 校验启动期 PCI DMA quiescence、当前 firmware/fabric lineage、所选 IOMMU backend 的有效 default-deny
状态和对应 reserved-memory policy 已完全处理；
无 IOMMU/不满足条件时 fail-closed。`PciDmaDomain` 的 map 请求只能指定 IOVA、MemoryObject 与页内 object offset，不能指定
物理地址；kernel 还按 DMA 方向检查 MemoryObject 权限、owned frame、page readiness 和完整页。BME enable 只有在 IOMMU domain
ready 后通过 domain capability 进入，unmap 则在 BME 关闭且所选 backend 的 page-table/cache invalidation 成功后才释放 backing reference。
`pcied` 的 `NAOS_PCIED_MSI_ROUTE_SMOKE` opt-in 会为 QEMU EDU 创建单页 DMA mapping，验证获准写入和未映射 IOVA fault，并
驱动/ack 两次 MSI。普通启动现在为匹配的 Virtio Block child 创建独立 domain；QEMU smoke 已验证有界 block write/flush/read 和 fault-source
检查。两条路径都是单一 QEMU 设备/配置的运行证据，不代表真实硬件、恶意 DMA 覆盖或 G11 已完成。

当前 `bind_root` 由受信任的 `pcied` 在验证 `PlatformPciTopology` generation、计算 MCFG intersection 并取到
`PciRootResourceAttestation` 后调用。`platformd` 只能经其专属的 `AcpiResourceAuthority` scope 提交 bounded `_CRS` window
table；kernel 检查 scope owner、完整 generation/root identity、唯一且精确匹配的 bus window、descriptor flags、range/translation
溢出及至少一个 memory window，再 mint 可 move、不可 duplicate 的 attestation capability。`PlatformPciTopology` 以
generation-bound、one-shot 方法把该 capability 转交给 `pcied`；`bind_root` 必须连同 capability 一起提交。kernel 再核对 attested
lineage 与请求的 root/bus window，并只在 MCFG containment 成功后将 translated memory windows 固化进
caller-PID-bound、不可转移的 `PciFabricAdmin`。`PciDiscoveryFabric` endpoint 自身仍没有 `TRANSFER` right；IPC 资源参数 admission
依据每个参数自己的 `TRANSFER` right，而不是借用 target capability 的 delegation right。

这证明 root window table 是由唯一可访问 ACPI executor 的 `platformd` 交给 kernel 校验并封装的权威证据，不证明 kernel 独立执行或
认证 AML `_CRS` 的语义正确性；该事实仍属于受信任 `platformd`。现有 attestation 只覆盖 host-bridge root windows，尚未覆盖
每个 PCI function 的 `_CRS` BAR 描述，不能据此开放 BAR assignment 或 driver MMIO mapping。平台/内核拓扑语义若需要独立于
`platformd` 的认证，仍需另外定义固件来源与可验证证据。

本期 `pcied` 只在启动时读取一次 platform topology，并保留该 service endpoint。SCI consumer failure 时，`platformd` 先调用
kernel `AcpiFixedHardware.report_sci_failure`，再推进 topology generation 并关闭该 incarnation 的 topology server connection；
kernel 在报告成功时立即永久撤销本 boot 的 `pci::topology_lifecycle`，若 `platformd` 在报告前退出，则 fixed-hardware owner-exit
路径执行同一撤销。该 lifecycle 绑定 `boot_epoch`、firmware/fabric generation 与首次 root bind 的 topology generation；旧
`PciFunction` 的 BAR/resource、grant、DMA-domain、route 等变更/授权方法以及新的 root-admin/function mint 都 fail closed 为
`ESTALE`/revoked，read-only config/identity 查询仍可用。IRQ route admission 在 function dispatcher、route preparation 前后均检查
该 lifecycle；已 mint 但尚未 bind 的 `InterruptSource` 不能在撤销后启用，而 mask/teardown 不受撤销阻断。因而 child 已持有被
`grant_driver` 转交的 function capability，也不能在拓扑失效后 mint 新 route。此为 SCI failure 的 kernel-authoritative、同 boot
终态撤销，不是通用逐对象 revoke；已经启用的独立 `InterruptSource` 不会被此标志主动销毁，仍需 owner teardown。
`pcied` 在 topology endpoint peer-close 后 fail-stop，退出会销毁其不可转移的 `PciFabricAdmin`/`PciFunction` handles；其
generation-bound RPC wait 与启动期 root bind/ECAM scan 可被 peer-close 取消，中断时销毁尚未发布的局部 capability tree。
`PlatformPciTopology` 在一个正常 service incarnation 内仍 immutable，没有同 endpoint generation-update/rescan API；未知 health bits
也 fail closed。预发布阶段曾将 `boot_epoch` 和 firmware snapshot generation 放进 `PlatformDeviceSnapshot`；当前协议把这对值折叠进方法级 `firmware_version` 字符串，`pcied` 原样比较并转发该字符串，再分别校验 namespace/topology/fabric generation。上线前所有 IDL revision 已统一重置为 1。
`InterruptSource` wait/ack 和 `PciDmaFaultSource.take` 也在方法级携带 `firmware_version`，`pcied` 在处理 pending/ack/fault 数据前与当前内核身份比较。
2026-10-21：预发布阶段的 `PciDmaFaultSource.take` batch 曾从 kernel 持有的 `function_identity` 携带 lineage；
`pcied` 在读取 fault batch 前逐字段对照 child 的 kernel-minted function identity。kernel batch、NaoIDL round-trip 和 stale-field
拒绝 regression 均通过。2026-10-21：`AcpiResourceAuthority.bind_platform_generation` 只接受已绑定 AML worker 的进程 owner；kernel
同时验证 generation 高 32 位等于该 kernel PID、低 32 位序号非零且 topology 序号晚于 AML namespace。首个完整 lineage 与
boot/firmware/fabric snapshot 原子绑定并永久冻结；重复提交只允许完全相同的 lineage。后续 PCI_Config root scope 与 root bind
均须匹配此 kernel lineage。`AcpiFixedHardwareInfo`、`AcpiSciEvent` 和
`AcpiSciAck` 现在都由 kernel 从该快照编码完整五代 identity；`platformd` 在处理 event 和 ack 前逐字段比较，不采信请求回显。
拓扑 lifecycle、NaoIDL canonical round-trip 和每代字段分别过期的 Rust rejection regression 均通过；2048 MiB headless QEMU
普通启动与 opt-in EDU + VT-d interrupt-remapping smoke 均验证新绑定先于 SCI claim，后者继续通过 child grant、DMA allow/deny
fault、source-close 后 fault drain、MSI ack 和 teardown。该次 fixture 未触发 SCI/GPE event；之后的 2026-10-25 GPE.2 fixture 已在正式
512 MiB 配置下验证 SCI event payload dispatch 与匹配 ack。真实硬件 event 行为仍待验证。2026-10-22：kernel 在 `exit_process` 早期先撤销 AML executor owner；只有已绑定 owner
匹配时才永久 invalidate 当前 boot 的 PCI topology，并撤销其 PCI_Config scopes，不依赖 SCI 或 transition owner 是否已经 claim。
新增的 “AML executor owner exit before SCI ownership” 回归验证旧 lineage 立即失效且 executor 不可同 boot 重绑；kernel 完整构建、
2048 MiB headless boot 与 EDU child-exit DMA/MSI smoke 通过。这样关闭 topology 已发布、fixed-hardware owner 尚未认领时的失效窗口。
G12 的身份生命周期验收项已完成：对外身份携带 `firmware_version` 字符串和适用的 namespace/topology/fabric generation；字符串编码的 boot 与 firmware snapshot 身份只由 kernel 解析，消费者原样比较/转发，kernel 对 AML owner 退出及 SCI failure 执行同 boot 永久撤销，generation 分配在序号耗尽时 fail closed。
在线 recovery/rescan、同 endpoint generation update 和新的 service incarnation supervisor 尚未实现；它们属于显式
platform recovery transaction 的后续工作。当前 peer-close fail-stop 不提供通用在线恢复服务。SCI consumer 失效后，`platformd` 在发布 degraded generation 并关闭 topology endpoint
后退出，让 kernel 的 owner-exit 路径清理 AML executor leases；不会把失效的 interpreter 留在后台等待，也不会自动重启或重放
`_REG/_INI`。kernel 在首次 AML worker 绑定退出后，将该 boot 的 executor binding 置为 terminally revoked；同一 boot 的新
`platformd` 必须在 namespace load/AML method 执行前被拒绝。当前恢复仍要求受控机器重启；新 boot 创建新的 executor 与
新 `firmware_version`。

本节标记为“后续”的接口仍是未来合同。开始实现新的跨进程服务时，必须拆入 `idl/system/*.naidl`，使用 canonical
little-endian wire、带外原子 resource disposition 与本文的 epoch/rights 规则；不得创建占位 NaoIDL 或 no-op endpoint。

### 8.1 后续：`AcpiExecutor`（`platformd` ACPI backend 私有模块合同）

第一阶段 `AcpiExecutor` 只服务 `platformd` 的 ACPI backend 内部，以 Rust trait/有界任务队列实现，不向 `pcied` 或任意应用公开，也不是独立
NaoIDL service。若将来拆分 ACPI executor process，本节的接口须原样升格为 NaoIDL；它不含 `evaluate(path, args)` 这样的生产接口。

| 方法 | 输入 | 输出 | 语义 |
| --- | --- | --- | --- |
| `initialize` | expected firmware generation、immutable `AcpiCompatibilityProfile` | AML namespace generation、feature flags | 首次加载 tables；不可隐式重放 |
| `snapshot_devices` | cursor、limit、allowed-entry profile | `AcpiDeviceSnapshot[]` | profile 仅允许固定的 `_STA/_CRS/_PRT` 等入口集；不声称其无副作用 |
| `get_pci_roots` | namespace generation | `PciRootBridgeDescriptor[]` | 返回 segment、bus window、ACPI device identity 和 routing facts |
| `refresh` | scoped device token、reason | new generation 或 domain error | 只允许 policy 定义的 hotplug/rescan 原因；第一阶段关闭 |
| `subscribe_events` | event mask | `EventStream` | 事件异步发送，不能反向同步调用 client |
| `prepare_power_transition` | S-state、request nonce | `SleepPlan` | 后期接口；不等同于直接关机 |

`AcpiCompatibilityProfile` 必须在 namespace/table 初始化前冻结，包含 `_OSI` claims/policy、`_OS`、`_REV` 与 `_PIC`
interrupt-model policy。它有 stable profile hash，记录在 `aml_namespace_generation` 中；普通 client、`pcied` 和驱动均不能
在启动后改变它。`_PIC` 以选定 interrupt model 恰好执行一次，且必须发生在任何 `_PRT` 求值前；它也使用受控 execution
profile。这样 firmware 不能因运行时 caller 改变兼容性答案而获得不同 namespace/`_INI`/routing 行为。

每次 namespace initialization、control-method evaluation、SCI work 与 power transition 都携带不可扩张的
`AmlExecutionBudget { opcode_fuel, native_access_fuel, max_call_depth, max_loop_iterations, max_namespace_objects,
max_package_bytes, max_buffer_bytes, max_heap_bytes, deadline }`。allowed-entry profile 同时声明允许的入口和各 address-space
operation policy；它不是“只读”标记。budget/profile 均由 `platformd` policy 在调用前选定，AML 或普通 client 均不能扩大。

`AcpiDeviceSnapshot` 使用 opaque `device_id + namespace_generation`，包含 canonical ACPI path、HID/CID/UID、已归一化状态及
有界资源列表。`device_id` 只能由 `AcpiExecutor` 产生；调用者不能通过猜测 path 获得更高权限。字符串 path 最大 255 bytes，
资源数量最大 256，单 snapshot 最大 64 KiB。AML `Reference`、内部 namespace pointer 和 region token 不得出现在 wire 中。

`_STA`、`_CRS`、`_PRT` 是 AML control method，不是“只读查询”；它们可调用其他 method、访问 OperationRegion 或写硬件。
allowed-entry profile 只是入口 allowlist，所有执行均按受控 AML hardware execution 处理。一切会执行 AML 的 method 都有单独的
`operation_budget`；超时、client cancel 或 client disconnect 只说明 client 不再等待，不表示方法、寄存器写或电源转换已回滚。
除 schema 明示为幂等且 policy 允许的入口外，client 不得自动 retry。

### 8.2 后续：`PlatformPciTopology`（`platformd` 的对外平台服务）

`platformd` 将 ACPI backend 的 AML 结果或 DT backend 的 DTB 结果转化为稳定、可缓存的、与 firmware 描述格式无关的平台视图。`pcied` 只能依赖本节的
`PlatformPciTopology` contract；它不得导入 `acpi` crate、持有 AML path、调用 AML method，或解析 ACPI/Device Tree bytes。
未来的 Device Tree provider 必须实现同一 contract，而不是让 `pcied` 增加 `if ACPI / if DT` 分支。

| 方法 | 调用者 | 结果 |
| --- | --- | --- |
| `generation` | consumers | current platform generation、health |
| `list_devices(cursor, filter)` | device manager | immutable `DeviceSnapshot[]` |
| `get_pci_roots(generation)` | `pcied` | opaque platform-root ID、segment、bus window、host-bridge resource windows 与关联 device ID；不含 ECAM physical address |
| `get_resources(device_id, generation)` | `pcied`/authorized consumers | immutable、normalized `ResourceSet` |
| `get_interrupt_routing(device_id, generation, cursor, limit)` | `pcied` | bounded typed `_PRT` route facts；GSI 或 link-device opaque ID，不暴露 AML path |
| `route_interrupt(function, pin, generation)` | `pcied` | typed `IntxRoute`；不暴露 `_PRT`、GSI raw authority 或 AML path |
| `set_power_state(device_id, target, budget)` | `pcied` | asynchronous transition outcome；`platformd` backend 决定 AML/firmware ordering |
| `negotiate_pci_features(root_id, requested, generation)` | `pcied` | immutable `PciRootFeatureSet`；ACPI backend 执行 `_OSC` query/commit，DT backend 应用等价 policy |
| `subscribe_changes(from_generation)` | consumers | ordered platform event stream |

`PlatformPciTopology` 只发布事实、操作请求和事件；不得把 `MemoryObject` table、`AcpiResourceAuthority`、region token、
physical address、ECAM mapping、raw AML result 或 ACPI-specific identifier 作为可操作资源转交。消费者必须以 generation
处理 stale snapshot；若 generation 已失效，返回 `STALE_GENERATION` 而非悄悄使用新拓扑。

`PlatformGeneration.health` 为 bitmask：bit 0 表示可继续只读发现的 informational degradation（例如可选 `_OSC` control
被拒绝）；bit 1 表示 AML execution deadline 已 poison 当前 executor，禁止新的 PCI discovery/bind；未知 bit 必须 fail closed。
`pcied` 不得因 root facts 仍在响应就忽略 bit 1。

ACPI backend 的 `negotiate_pci_features` 对每个 root 执行有界 `_OSC` query 序列；收到 Capabilities Masked 时，仅按固件返回值
收窄 support/control mask 后重试，任何无进展或越过 33 次 query 上限的序列都 fail closed。只有 mask error 清除且返回 mask
与收窄后的本次 query 一致后，才以该最终 mask 执行 commit，不能把已被 query 拒绝的原始 control mask 再送入 commit，遵循
[ACPI 6.6 §6.2.12.1](https://uefi.org/specs/ACPI/6.6/06_Device_Configuration.html)。它把
query/commit support、query 阶段控制 grant、最终 granted/denied/masked bits、两阶段错误、profile hash 和 epoch 固化为
`PciRootFeatureSet`。当前 `PciRootFeatureSupport.segment_groups`（bit 3）是唯一实现并声明的 OS support；native control
mask 为零。kernel 只能为 firmware 已授予 OSPM ownership
的 native feature mint 对应 admin capability；`pcied` 不得仅因自身支持 hotplug/AER/MSI 等功能就启用它们。

所有 externally visible object/event 必须携带 opaque `firmware_version` 字符串；该字符串由内核解析为 boot 与 firmware snapshot 身份。
它不替代另外三项单调 generation：`aml_namespace_generation`
（interpreter init/recovery）、`platform_topology_generation`（platformd 发布 facts 变化）和 `pci_fabric_generation`
（PCI discovery/bind/rescan 变化）。opaque device/root IDs 仅在其 epoch+generation lineage 内有效；任何 service restart、
namespace recovery 或 fabric rescan 都不得使旧 `device_id=7,generation=4` 意外重新合法。

### 8.3 本期 root binding；后续生产 function policy 与 driver binding

MCFG 只描述 ECAM allocation，不是 host-bridge resource policy。当前 `pcied` 取得 `PlatformPciTopology.get_pci_roots()` 和对应的
one-shot `PciRootResourceAttestation` 后，以 `(segment, bus-window)` intersection 调用 `PciDiscoveryFabric.bind_root`；kernel 校验
attestation lineage、validated host windows 与 MCFG containment 后 mint `PciFabricAdmin`。`PciDiscoveryFabric` 自身不能直接 map
ECAM；admin capability 固化 intersection、root memory windows、owner PID 与 epoch/generation lineage。`PciFabricAdmin.open_region`
只返回该范围的 read-only ECAM `MemoryObject`；`open_function` 只接受
该 root 的 bus window 内 BDF，并返回不可转移、owner-bound 的 `PciFunction` 身份 capability。`pcied` 只在 ECAM scan 已发现
该 BDF 后调用 `open_function`；kernel 验证 BDF 的格式/范围，但不自行读硬件探测 function presence。legacy CF8/CFC 不支持。

最终发布的 root bridge 必须匹配同一份 `DeviceSnapshot` 的 `device_id`、AML path 和 namespace generation，且该 snapshot 的 `_STA`
状态已求值并同时报告 present 与 functioning。失败、畸形或 lineage 不匹配的 `_STA` 不产生可扫描 root。使用设备快照中的同一次
`_STA` 结果，禁止为了 root discovery 再执行一次可能有状态副作用的 AML method。启动期 provisional root/config selector 只用于
受控的 AML 初始化求值；最终 root scope、PCI_Config selector、`_OSC` negotiation、resource attestation 和对 `pcied` 的发布都在
状态筛选后进行。`pcied` 不得根据 provisional root 或 MCFG 单独扫描。

| 方法 | 调用者 | 结果 |
| --- | --- | --- |
| `PciDiscoveryFabric.bind_root(index, platform_root_id, generations, segment, bus-window, attestation)` | `pcied` | kernel 校验 kernel-minted root attestation 与 MCFG intersection 后 mint scoped `PciFabricAdmin` |
| `PciFabricAdmin.open_region()` | `pcied` | 该 admin root 固定范围的 read-only ECAM mapping |
| `PciFabricAdmin.open_function(bdf)` | `pcied` | root window 内、已由 `pcied` scan 发现的 owner-bound `PciFunction`；其 `create_dma_domain` 仍由独立 kernel IOMMU policy gate |
| `PlatformPciTopology.take_device_resource_attestation(device_id, root_id, generation)` | `pcied` | 一次性转交 platformd issuer 从 root capability 派生的单设备 `_CRS` memory-range attestation |
| `PciFunction.bind_device_resources(device_id, attestation)` | 仅 owner `pcied` | kernel 重验 root containment 并通过 ECAM bridge 链解析 `_ADR`，只接受解析到该 function BDF 的 attestation |
| `PciFunction.read_config(offset, width)` | owner `pcied`；driver 需 `read_config` profile 位 | capability 固定 BDF 上自然对齐的 8/16/32-bit config 标量读取；不返回 ECAM view，不写 config |
| `PciFunction.open_bar(index)` | owner `pcied`；driver 需对应 `bar_mask` 位 | 只读 assigned BAR config；要求 base/size/page cover 与该 function 的 attested `_CRS` 完全匹配，返回 map-only `DEVICE_MMIO` MemoryObject；不写 BAR size probe |
| `PciFunction.create_dma_domain()` | 普通 owner `pcied`；获 grant 的 child 需 `create_dma_domain` profile 位 | Intel VT-d 与 AMD-Vi 均接入 vendor-neutral backend，并在 PCI quiescence 与所属 unit fault/event policy 通过后返回该 BDF 的独立 `PciDmaDomain`；AMD fault delivery 与 owner teardown 已由 opt-in QEMU EDU child 验证 |
| `PciFunction.route_interrupt()` | 普通 owner `pcied`；获 grant 的 child 需 `route_interrupt` profile 位 | 安装准确 requester-ID 的 IRTE、配置为 disabled MSI/MSI-X，并返回 owner-bound `InterruptSource`；Virtio Block production child 与 EDU/Rocker opt-in smoke 均验证实际 interrupt delivery/ack；不授予通用 config write |
| `InterruptSource.bind/wait/set_mask/ack` | 仅创建它的 owner；wait 同时要求 capability `WAIT` right | typed enable、readiness snapshot、mask 和序列化 ack；未知 hardware outcome 后拒绝重试并 quarantine |
| `PciDmaDomain.map_page/unmap_page/set_bus_master/close` | 创建时绑定的 process owner；每次调用校验 caller PID | IOVA 到 owned MemoryObject frame 的隔离映射、停止 BME 后 unmap、受策略约束的 BME 与 orderly teardown；handle move 不重绑定 owner |
| `PciDmaFaultSource.take(limit)` | 与该 function 的 DMA domain 同一创建 owner；每次调用校验 caller PID | readiness-signaled、requester-scoped 的 bounded fault records、software queue dropped count 与 unit-wide hardware overflow flag；非 owner 请求不消费 queue |
| `PciFunction.grant_driver(profile, Process)` | 仅 owner `pcied` | 接受显式 Process capability（不接受 PID），拒绝调用者自身、已启动或已退出 target；profile 限制 typed read/DMA/IRQ rights 与 6 个 BAR index。返回预置 one-shot `PciFunction` 的有界私有 ServiceDirectory，并授予仅限本地 registry 的 register right；不转交 `PciFabricAdmin`、全局 publish、通用 MMIO/config authority 或 `_CRS` mint 权限 |

当前 kernel `route_interrupt` 已有 MSI-X 配置路径：优先选用 MSI-X capability，只有 capability 明确不存在时才回退 conventional MSI；畸形或不可读的
MSI-X capability 会 fail-closed。kernel 使用 PCI function 持有的单设备 `_CRS` authority、已验证 root windows 与 assigned BAR base/length
解析 Table/PBA，再为 Table 的 page cover 建立 kernel-owned exclusive BAR lease 和 uncached kernel mapping；该 mapping 不作为通用 MMIO capability
交给调用方。MSI-X capability/Table/PBA decoder 与 page-cover planner 都有边界、可写属性和 root-containment 测试；kernel BAR lease helper 也有
kernel-owner 生命周期测试。`q35_pci_msix_route_boot` 使用 MSI-X-only 的 QEMU Rocker function，测试其 BAR0 liveness、拒绝通用映射 BAR1，
并通过 `TEST_IRQ` 触发 vector 0；测试仅在空 default-deny `PciDmaDomain` 下临时开启 BME，随后 ack、停止 BME、关闭 domain 和 route。它验证 QEMU
虚拟设备上的完整 MSI-X/IRTE 运行路径，不替代真实硬件或异构设备验证。
`PciFunction.open_bar` 现拒绝把 MSI-X table 所在的整个 BIR BAR 作为通用 MMIO view 暴露给 client；MSI-X 描述畸形时 BAR 打开也
fail-closed。这样暂时不能使用同一 BAR 上的普通 device registers，但不会给 client 留下绕过 typed `InterruptSource` 改写消息表的路径。
常规 MSI 的 prepare/enable/restore 都会拒绝同一 function 上已启用的
MSI-X，防止固件遗留两种机制同时生效。注入 config/table I/O 回调的 MSI-X transaction helper 会 mask 全部 vector，只配置 vector 0，按
function-mask 顺序启用/停用，并在未知写结果时拒绝重试；它现已接入 `InterruptSource` prepare/enable/disable/restore 及 IRTE teardown。
独立 `q35_pci_msix_route_boot` 使用 QEMU Rocker（MSI-X-only）和固定双 BAR `_CRS` fixture，验证 table BIR 不向通用 `open_bar` 暴露、kernel 私有
BAR1 table mapping、BME 仅经空 default-deny `PciDmaDomain` 启用、TEST_IRQ 触发 vector 0、实际事件 ack 与按序 teardown；QEMU VT-d trace 记录
`addr 0xfee000XX/data 0` 的 MSI-X memory write 并命中本次 IRTE。结合 callback transaction、MSI-X table mapping plan、kernel BAR lease 与 kernel 编译/单测，
现已覆盖 QEMU 端到端运行时路径，但不替代真实硬件、多型号、固件和生产 driver 生命周期验收。route 仍先注册 kernel handler/保留高于 drain IPI priority
class 的 vector，再解析 function 的 DMAR source、安装并 IEC-invalidate IRTE，最后配置 disabled MSI 或 MSI-X source；只有
`InterruptSource.bind` 才启用 source。`wait` 使用 capability `WAIT`/epoll readiness，RPC wait 只读 coalesced sequence；mask
关闭并 mask MSI 或 MSI-X，ack 只清除本地序列事件。teardown 先停用 source，在目标 LAPIC drain vector 的 IRR/ISR，退役并 IEC-invalidate
IRTE 后再次 drain，再恢复原 MSI config 或 MSI-X table/control；只有该序列和 handler unregister barrier 完成后才释放 vector。任一步 hardware outcome
unknown 时注册表、vector 和 static tombstone handler 会保留/quarantine，且不会对该 route 自动重试。普通 Virtio Block child
现获 `route_interrupt`，以 vector 0 的 MSI-X readiness 等待 used-ring completion，同时监听 DMA fault source；QEMU smoke 验证
真实 BlockDevice I/O 在该中断路径完成。opt-in EDU smoke 会先为两个 function 分别完成 child-driver grant/bootstrap，再由各 child 执行获准单页 DMA、未映射 IOVA 写 fault
检查、bind route 并投递/ack 两次 MSI，再按序停止
BME 前通过 `PciDmaFaultSource` readiness 读取并核对 VT-d fault record，然后停止 BME 并撤销资源。AMD memory-IOPF delivery、BME
disable 与 owner teardown 已由后文记录的规范修正 QEMU 11.1.1 fixture 验证，因此 G11 核心门槛已通过。系统化恶意 DMA 验证、生产
异构 driver binding 与真实硬件覆盖仍未完成，属于更广的生产化范围，不应再表述为 G11 核心门槛未验收；真实硬件测试不是验收前置条件。
另一个 opt-in
`q35_virtio_block_terminal_dma_fault_boot`
使用生产 `virtioblkd` 注入 IOVA `0x20000000` 的真实 VT-d fault，验证 `BlockDevice.write` 返回 EIO、child 以非零状态退出、PCIED 移除 catalog
provider、36 个 DMA mappings 被清理并完成 MSI route teardown。失败路径先从 readiness set 注销 MSI-X source，再显式 drop route capability，
使 teardown 在 child 的正常调用上下文执行；CTest 在 2048 MiB headless Q35 中通过。

function selector 的 `bus > 255` 返回 `ERANGE`（`OUT_OF_RANGE`），无效 device/function 或 malformed root range 返回 `EINVAL`
（`INVALID_ARGUMENT`），而合法 BDF 落在 bound root 之外返回 `EACCES`（`ACCESS_DENIED`）；三类结果不得折叠。

当前 root capability 已绑定 kernel 校验过的 host-bridge `_CRS` memory windows。per-device 路径现由 platformd 保留完整
address-space 描述并生成有界 device evidence；kernel 只允许 root attestation 的原始 issuer mint child capability，再由
`PlatformPciTopology` 按 device ID/root/generation 一次性转交 pcied。`PciFunction.bind_device_resources` 会消费该 capability，
重验 root containment，并沿 ECAM 读取 PCI-to-PCI bridge bus numbers，将 child capability 中的完整 `_ADR` 路径解析到唯一 BDF；
pcied 传错设备 capability 时 fail-closed。`PciFunction.open_bar(index)` 只读取已分配 BAR config，不写 all-ones 做 size probe；
它要求 BAR base 精确匹配该 function 的 attested `_CRS` window、aperture 为对齐的 2 的幂且完整页范围位于 root window，随后经
physical-resource ledger mint exclusive `pci_bar` lease 并返回无 physical address 的 map-only `DEVICE_MMIO` view。view 强制
shared/non-executable，绕过 `MemoryObject.read/write` IPC；写 map 权限由 `_CRS` `_RW` 决定。当前 Q35 discovery smoke 未遇到
关联 endpoint 的 fixed consumer memory `_CRS`，所以普通启动在该固件上不会 mint BAR authority；opt-in smoke 注入固定 `_CRS` 后，
child 已有 hardware-backed BAR runtime 验收。`PciFunction.read_config` 仅做单个 BDF 的 aligned 1/2/4-byte volatile ECAM read，在 kernel config lock 下
经现有 exclusive ECAM resource lease 映射并立即撤销；错误宽度、越界、误对齐和 stale root lineage 分别 fail-closed，不提供
通用 config mapping 或写操作。`PciDmaDomain` 和 `InterruptSource` 在普通启动中均由 PCIED 转交给固定 Virtio Block child；
QEMU opt-in smoke 验证该 child 的 MSI-X 路由与 block I/O。每个 child 只持有一个 function。

driver-facing `PciFunction.grant_driver` primitive 已有 schema、profile validator 和 owner-bound proxy mint。profile rights 是
read-config、DMA-domain creation、interrupt-route 三个位；BAR 以独立六位 mask 表达，且只有已有匹配 `_CRS` attestation 才能
授权。target 是 Process capability，不是 PID；内核只返回单 function 的 private ServiceDirectory。opt-in EDU smoke 已验证
PCIED 对两个 `PlatformPciTopology` compatible ID 都选择 opt-in `edud` catalog entry，再通过 deferred child lifecycle 分别将私有目录
放入标准 ServiceDirectory bootstrap。PCIED 在启动任一 child 前先完成全部 image read、Process prepare 与 function grant；之后逐个
等待并确认 `Process.start` 回复，再用独立 Tokio readiness waiter 并发收集 child exit status。启动/退出错误会被报告，已启动的
child 不会被描述为已回滚。两个 child 各自检查 config/BAR0 权限、BAR1/相邻 URI 拒绝和正常退出，并各自用
获准的 DMA/IRQ rights 创建并关闭 domain/source/route，完成两次 MSI ack、合法 DMA 与 fault 检查。
但目前只有固定 Virtio Block catalog，没有通用生产 driver catalog；异构设备分配策略或热拔插 lifecycle 仍未实现，因此完整生产
grant/lifecycle 尚未完成。当前 opt-in
QEMU 集成验证两个同型 EDU function 的 preflight、grant、独立 child 与 teardown；这不构成生产策略或通用多型号设备管理。任何 grant 只能包含协议
明确列出的 typed config reads、按 index 收窄的 BAR capability 与 `InterruptSource`；不能以 caller payload 中的 BDF 绕过
function scope。通用 PCI config write、BAR size probe、bridge bus-number 更改和 trusted-driver override 仍未开放。MSI/MSI-X
route 的建立、enable、mask 与 teardown 仅由 kernel typed operation 执行；driver profile 只授予 `route_interrupt`，不授予通用 config write。
这些操作使用已定义的 owner、顺序与 unknown-outcome/quarantine 语义。特别地，
`COMMAND.BUS_MASTER_ENABLE` 的写入要求 `PciFunction` 已绑定 `DmaDomain`/`IommuDomain`；没有 IOMMU 的机器默认拒绝，
只能由显式 trusted-driver policy 放行。MSI/MSI-X 的 destination/vector/message 由 kernel mint 的 interrupt-message
capability 决定，driver 不得直接写入任意 destination/vector。

该 interrupt capability 不能只是 `pci_msi_message.hpp` 编码出的 APIC address/data 数值。MSI/MSI-X route 只有在该 function
所属 IOMMU unit 同时具备已启用的 translation/cache invalidation 与 interrupt remapping 后才能建立；Intel backend 的 QI/IR
和 AMD backend 的 command/IOTLB/interrupt-table invalidation 各自遵循其架构规范。Intel IRTA 或 AMD DTE/interrupt-remapping
table 初始项均为 deny/invalid；IRTE 必须绑定准确的 PCI requester-ID，并由 kernel 选择 destination/vector。MSI/MSI-X config 的启用、mask 与 teardown
由 kernel 的 typed operation 执行，driver 只持有 owner-bound `InterruptSource`，不取得通用 PCI config write 或可伪造的裸
message capability。创建、修改和撤销 IRTE 后必须执行 backend-defined interrupt-table invalidation，并等待完成及在途 interrupt drain 后才能复用
vector/index；完成状态不确定时 route 与 vector 均 quarantine，不得自动重试。对应 backend 的 invalidation 或 interrupt
remapping 缺失/失效时返回 `NOT_SUPPORTED`/degraded，不得回退为无 interrupt-remapping 的 xAPIC message。Intel 与 AMD 的
register/table encoding 只能由各自 backend 实现，driver 和通用 PCI code 不得分支读取 vendor hardware state。

Interrupt remapping 的安全门槛相同，但启用粒度必须遵循各自硬件规范。Intel VT-d 的 IRE 是 unit-wide 模式；启用前，kernel 必须先为
仍在使用的 IOAPIC/HPET source 安装并验证匹配的 remappable route，或保持这些 source masked，不能仅因一个 PCI function 请求 MSI 就打开 IRE。
若无法把该 unit 内所有活动 producer 纳入受控路由，Intel unit 的 IR/MSI capability 必须保持 unavailable。
AMD-Vi basic interrupt remapping 则由每个 requester 的 DTE `IV`/`IntCtl` 与该 DTE 的 interrupt-table pointer 控制，没有 Intel 式 unit-wide IRE
开关。AMD route 必须先构造 source 专属 IRTE、写入并失效对应 DTE/table，再把该 source 的 DTE 从 abort 状态切换到 remapping；未纳入 route
的 PCI source 继续 abort。IOAPIC special requester 按 MADT/IVRS 身份单独解析，RTE 保持 masked 时才从 IVRS pass-through 转为 abort/remap，
并在退役后恢复其 pass-through DTE；HPET special requester 仍须保持受控的 unmapped forwarding 或保持 source masked，不能把 PCI requester 的默认拒绝策略放宽。Intel VT-d PCI MSI IRTE 使用 SVT=verify SID/SQ、SQ=all 16 bits，并绑定准确 requester ID；xAPIC
目标按 IRTA 的 EIME 模式编码，reserved bits 保持为零。AMD-Vi 按 AMD IRTE 格式验证 DeviceID、vector、destination、delivery mode 与
reserved fields。公共路由 handle 不暴露 entry index 或 message encoding；两种 backend 都必须在发布 route 前完成对应的 interrupt-table
cache invalidation。公共 provider 的 activation/readiness 操作表达后端所需的策略门槛，不得假定两家都存在同一种全局硬件开关。

### 8.4 统一错误、取消和并发规则

稳定协议至少区分 `ACCESS_DENIED`、`INVALID_ARGUMENT`、`OUT_OF_RANGE`、`NOT_SUPPORTED`、`NOT_READY`、`STALE_GENERATION`、
`RESOURCE_REVOKED`、`CONFLICT` 和 `WOULD_BLOCK`。这些是 protocol/domain result；`NOT_DELIVERED`、`OUTCOME_UNKNOWN`、
deadline 与 cancellation 仍遵循 Invocation/Responder 的 transport outcome，不能被折叠成成功或普通 `EIO`。
PCI DMA 后端若已触及设备/IOMMU 状态、但无法确认命令或失效操作是否完成，必须返回
`OUTCOME_UNKNOWN + HARDWARE_OUTCOME_UNKNOWN`（`protocol_error = 0`）；即便底层同时给出普通 status，也不能把它当作确定的
domain error。只有硬件状态可确定时才返回 `protocol_error`/domain result。调用方可以把未知结果映射为 `EIO` 供 POSIX
接口使用，但必须保留 result frame 中的 transport outcome/reason，且不得盲目重试。
当前 native RPC 的 `protocol_error` 以负 errno 暴露：已分类路径使用 `ACCESS_DENIED -> -EACCES`、
`STALE_GENERATION -> -ESTALE`、`RESOURCE_REVOKED -> -EIDRM`；`WOULD_BLOCK` 才使用 `-EAGAIN`。新增路径必须保留这些
结果差异，不能用 errno 的方便性把 generation 失配、资源撤销、权限拒绝或硬件结果未知混为一类。SCI drain/ack 的部分失败
现在按 stale sequence、revoked owner state、conflict、暂时无事件和硬件失败分类；claim 与 GPE mask 也有 typed result。
`report_sci_failure` 与 sleep/reset prepare/commit 也返回 typed admission/ownership result。commit 的 `outcome` 字段仍表达
硬件 transition 已执行后的结果（包括 outcome unknown），不能和 Invocation transport outcome 混淆；启动期 `start_sci`
布尔值只用于 kernel 初始化，不是 RPC 错误面。
prepared sleep/reset ticket 现在支持 owner-bound `abort_transition`，prepare 响应编码/投递失败和 owner 退出会释放状态；
在 kernel 时钟声明跨 CPU 单调时，未完成 ticket 也会在 30 秒后失效，不能再被 commit 或阻塞后续 transition。若时钟不具备
跨 CPU 单调保证，则不启用基于时间的回收，避免比较不同 CPU 的本地时间戳；调用方仍可显式 abort，owner 退出也会清理。

ACPI executor 采用有界工作队列。第一阶段所有 AML control method（包括 `_STA/_CRS/_PRT`）都在单一 interpreter ordering
domain 串行执行，不以方法名推断“只读”。只有在 Rust interpreter 的同步模型、完整 transitive call graph 与 region
access profile 证明无副作用/无共享状态冲突后，后续版本才可有限并行。调用图必须保持单向：

```text
pcied -> PlatformPciTopology (platformd) -> ACPI backend (executor/policy modules) -> kernel
kernel --event--> platformd ACPI backend --async platform event--> pcied
```

`platformd` ACPI backend、kernel hardware provider 和 `pcied` 均不得产生反向同步 call。这样能避免 AML 线程持锁等待
PCI policy、同时 PCI policy 等待 AML 的死锁；是否跨进程不改变这一约束。

### 8.5 Intel VT-d 与 AMD-Vi 硬件后端抽象

kernel 的 IOMMU owner 提供单一 vendor-neutral controller contract；Intel VT-d 与 AMD-Vi 实现为独立 backend adapter，
各自拥有 firmware inventory parser、register access、unit state、page-table/DTE encoder、command/invalidation、fault/event log、
interrupt-remapping table 与 uncertain-state quarantine。`arch::ACPI`、PCI capability dispatch、`PciDmaDomain`、
`PciDmaFaultSource`、MSI/IOAPIC routing 与 `PciFunction` 只能调用公共的 requester/domain/page/fault/interrupt operations；
这些层不得包含 vendor-specific header、寄存器 offset、table entry 或错误 enum。

公共 backend contract 至少包含：初始化并证明所有选中 unit 的 default-deny；按 segment/requester ID 唯一解析 unit；创建、
映射、撤销和销毁 owner-bound DMA domain；停用 BME 后才 unmap；为 PCI MSI 与 IOAPIC source 安装/撤销 source-validated IRTE；
执行 backend-specific interrupt-remapping activation policy（Intel VT-d 是 unit-wide IRE，AMD-Vi 是 requester DTE/IRTE 生命周期）；
配置、drain、ack fault/event source；报告每个可能已触及硬件状态的 operation 是否
`complete`、`failed-before-touch` 或 `outcome-unknown`。公共 domain handle 只保存 opaque backend token，opaque state 只由
创建它的 backend 解释。boot inventory 中每个有效 unit 只绑定一个 provider；overlap/ambiguous ownership、未知格式或无法
满足 reserved-memory semantics 时整项 admission fail-closed，不按 CPU vendor 猜测，也不把一种格式交给另一种 provider。

同一组 vendor-neutral conformance tests 必须驱动两种 backend 的 fake register/memory callbacks，覆盖 default-deny、权限与
地址宽度、domain/page lifecycle、所有可见 invalidation、fault overflow、interrupt remap enable/disable、unknown completion 与
quarantine。随后由 QEMU Intel VT-d 与 AMD-Vi fixtures 验证 firmware inventory、DMA transaction、MSI delivery 和 teardown；真实
硬件测试不是验收前置条件。单独通过 C++ fake-register tests 不能算作 backend 支持。

## 9. 性能与资源上限

本 PRD 所有正式 QEMU 验收启动都显式使用 `-m 512`；历史记录中使用更大 guest 的结果不代替 512 MiB 配置下的回归。

直接映射的 MMIO/ECAM 与 thread-bound SystemIO port lease 都避免每次 scalar access 的 IPC。TSS bitmap 无法区分 `in/out`，
因此内核直接租出完整 port range；当前只读 profile 在解释器 token 层限制 AML 写操作。各数据面规则如下：

- `SystemIO`：每个唯一 region 一次 lease admission；之后 `in/out` 都在绑定线程上直接执行，为 0 RPC。当前只读 profile
  的 AML write 经 access-token mode gate 拒绝。
- `SystemMemory`：每个通过 page-cover 判定的唯一 `(base, length, cache type, protections)` 一次 region grant 和一次 map；
  其后 load/store 为 0 RPC。page-cover 不能安全映射的 region 直接拒绝，不提供逐次 scalar IPC 后备。
- `PCI_Config`：仅支持 ECAM。`pcied` 可按 root/bus chunk 缓存 mapping 来避免按 function 反复映射；driver 只获得一个
  4 KiB function page。ACPI executor 可按已核验 BDF 缓存 mapping。其后 config load/store 为 0 RPC。
- `pcied`：单个 service incarnation 最多登记 4096 个 PCI function。超限时 discovery 整体失败并释放此前构造的局部
  capability tree；不得发布或记录为成功的部分 inventory。该上限同时限制内存增长与逐 function kernel capability mint RPC 数。

创建 capability、map/unmap、PCI scope 扩张、lease revoke 和事件订阅仍是低频控制面 RPC。SystemIO port access 与通过安全
page-cover admission 的 MMIO/ECAM access 都留在用户态数据面。

直接数据面不能做逐操作 kernel audit。审计记录 grant、map、revoke、owner、范围、cache/protection 与失败事件；需要
值过滤、逐操作记录或 rate limit 时必须使用专门 mediated protocol。所有队列、结果、事件流、table snapshot 和 audit ring
都要有固定 admission limit；超限返回 `WOULD_BLOCK` 或 `RESOURCE_EXHAUSTED`，不能让 firmware 提供的输入推动无界 kernel
allocation。

Rust interpreter 的 `operation_budget` 必须实现为执行状态内的 fuel，而不是只有 wall-clock watchdog：每解释一个 opcode、
method invocation 与 native region access 都扣除 fuel，并独立限制 recursion/call depth、loop iterations、namespace
object count、package/buffer allocation 和总 heap。`max_heap_bytes` 不能只是一条单调递增的记账计数：持久 AML 对象和当前
work-unit 的临时工作集必须分别计入上限，临时 reservation 在对象释放时归还；namespace/B-tree clone 和输出 scratch 都必须
在分配前有界。wall-clock fallback 是每个顶层 AML method 10 秒；嵌套 AML 调用共享同一
deadline，下一次顶层方法获得新窗口，watchdog 时钟每 1024 个 opcode 采样一次以避免逐 opcode syscall。deadline failure 在该
namespace execution state 中 sticky：后续 AML work 立即拒绝，`platformd` 发布 degraded，并禁止自动 retry/AML 重放。fuel 或
deadline 耗尽都保留“此前硬件操作可能已执行”的 outcome；强制终止解释器不能被描述为安全恢复，因为硬件可能已处于中间状态。

## 10. 生命周期、故障与电源

`platformd` ACPI backend 的 interpreter failure、namespace corruption 或 watchdog 失败后，`platformd` 发布
`firmware-unavailable`，`pcied` 停止新枚举、分配、配置和 hotplug 操作。若 panic 使整个 `platformd` 退出，kernel 的
AML executor owner-exit 路径立即撤销本 boot 的 PCI topology 与 PCI_Config scopes；`pcied` 在 topology peer-close 后
fail-stop 并释放本进程的 PCI authority。当前 early-boot launcher 只启动一次 platform service，没有守护进程自动重启它；
已授予驱动的既有 BAR/IRQ capability 仍受原有 capability 范围约束，但不会获得新的硬件权限。

不得由 supervisor 自动重启 `platformd` 或重建其 AML task 后无条件重放 `_REG/_INI`：这些方法可能已有不可逆副作用。
恢复是显式、可审计的 platform recovery transaction，建立新 namespace generation 并撤销旧 region/event token。
第一阶段推荐恢复动作是受控重启机器，而不是在线重建。

S5/reboot 请求现在经 `PlatformPowerControl` -> platformd policy -> kernel `prepare`/`commit` typed transition 传递；S5
只读取固定 `\_S5` 静态 Package，reboot 使用 FADT reset register，最终硬件写入只在 kernel。kernel ticket lifecycle 支持
owner-bound abort、owner-exit cleanup 与在跨 CPU 单调时钟可用时的 30 秒失效。`/bin/poweroff` 与 `/bin/reboot` 已迁移到该服务。服务先独立
认领 fixed-register transition owner；SCI 未认领且保持 masked 时仍可执行与 AML 无关的 FADT reset handoff。若 `_S5` 缺失或无效，
power-off 方法单独 fail-closed，不影响 reboot 方法发布。静态 `_S5` Package 查询不因 namespace 中其它设备使用 Global Lock 而拒绝。
若 SCI 正在处理事件则 transition prepare
拒绝；SCI degraded 时 commit 必须重新成功 mask 硬件 source 才可写寄存器。`poweroff` 收到 transition reply 仍表示机器未实际断电，
命令会报告 outcome 并返回失败；硬件写入成功不能证明电源已关闭。QEMU 固件中的 Global Lock dependency 由 typed FACS/SCI backend
处理，不会单独阻止 SCI claim；Global Lock `_GLK` 设备发现另由 `q35_acpi_global_lock_hid_boot` 验证。新增的 opt-in
`boot_smoke_platform_power_reset` 使用 Q35
FADT Reset GAS、`/bin/reboot` 和 `-no-reboot`，已验证 typed reset 调用使 QEMU clean-exit；默认 `pc` QEMU machine 未提供 Reset GAS，
所以该测试显式选择 Q35。它是虚拟平台路径验收，不替代真实硬件 reset/S5 验证。
同一 SCI、PM register 或 power button 不得由 ACPICA 与 `platformd`/typed kernel transition 双重拥有。

## 11. 安全与验收标准

实现前每个协议必须有 NaoIDL schema、rights matrix、resource transfer matrix、最大值和错误语义。至少验证：

- 伪造 physical address、region token、table id、BDF、generation 或 resource descriptor 均不能扩张权限。
- 普通进程无法通过其 ServiceDirectory view resolve `AcpiResourceAuthority` 或 `PciFabricAdmin`。
- `SystemMemory` 请求不能读取 normal RAM、kernel/用户页、table snapshot 或受保护 firmware 区。
- unaligned/cross-page region 仅在完整 `page_cover` 同属一个安全 lease 时 direct-map；否则拒绝，且 token 无法越过 logical byte range。
- FirmwareRoot graph 能在不使用物理地址的情况下重建 canonical root、FADT -> DSDT/FACS relation、DSDT-first 与有序 SSDT load。
- `rust-acpi-naos` 对三种 built-in region dispatch 均使用 token/error path；测试证明 raw Handler 无 fallback、lease revoke 与 `_REG`
  activation/deactivation 使 token 失效。
- 畸形 table/AML 的 corpus 与 fuzz 测试不会使 `platformd` ACPI backend panic、越过 allocation/fuel 边界或让 interpreter
  继续使用已 revoke token。
- range overflow、unaligned access、宽度不匹配、cache-type conflict 和 stale capability 都被拒绝。
- opcode/method/native-access fuel、call depth、loop、object/package/buffer/heap 上限可观察；每个高层 AML work unit（包括设备
  snapshot、PCI root discovery、region activation/deactivation）的 opcode、loop、native access 与 deadline 须跨顶层方法累计；
  fuel 耗尽不被标注为 rollback 或可自动 retry。
- 发送 capability 缺少 `TRANSFER`，或接收方请求更大 rights 时，资源事务整体失败且没有部分安装。
- `platformd` ACPI executor crash/timeout 后没有自动 AML 重放；新 PCI/IRQ 授予停止且客户端可观察 degraded 状态。
- kernel 与 `platformd` ACPI executor 不会同时注册 SCI/fixed event 或控制同一 power path；SCI 的 crash、overflow、stale-seq ack
  与 repeated rearm failure 不产生 interrupt storm。
- FACS page-cover 和 kernel-owned fixed ACPI inventory 永远不会经 generic region lease 暴露；FACS Global Lock 由 kernel 原子协议仲裁，
  SCI wake、超时、释放通知失败及 execution-thread/owner 退出均有明确 outcome；QEMU `_GLK` 设备的 `_STA/_INI` 与 HID/CID/UID snapshot 成功。
- 在 writable PCI phase，Bus Master enable 缺失 DmaDomain/IOMMU（或明确 trusted-driver override）时被拒绝，MSI/MSI-X destination/vector
  只能由 kernel minted message capability 提供。
- QEMU ACPI/PCI 启动 smoke 能完成 table snapshot、namespace discovery、只读 PCI enumerate；同时有恶意 table、畸形
  resources、取消/超时、背压和 capability attenuation 的行为测试。

## 12. 分阶段实施门槛

以下是最初的实施路线图，保留其阶段边界作为设计历史，不是当前实现状态。后续工作已进入 Phase 6 并超出 Phase 5 的只读 PCI 范围；
当前完成度以本节之后的 G1–G15 门槛记录和前文当前架构描述为准。

1. **FirmwareRoot 基础**：kernel 验证、复制并发布 FirmwareRoot、object graph、FADT edges 和确定的 AML load order；没有用户态
   physical mapping。
2. **离线 interpreter**：冻结 `AcpiCompatibilityProfile`；`platformd` ACPI executor 以 pinned `rust-acpi-naos` fork 建
   namespace、执行 fuel/heap/depth limits，并识别 Global Lock/FACS 依赖；需要硬件锁的 AML 通过 typed host callback 仲裁。
3. **受控 AML 硬件执行**：在 native region-dispatch 层引入 token admission、安全 page-cover-or-reject 与 direct I/O lease、
   AcpiFixedHardware 与 SCI mask-drain-ack-rearm；所有执行 profile 与硬件副作用可观察。
4. **平台服务**：`platformd` 发布 epoch/generation-based、firmware-neutral `PlatformPciTopology`。ACPI backend 在受控
   hardware-execution profile 下执行 `_STA/_CRS/_PRT` 生成结果；无 raw AML API，且不称这些方法为只读。
5. **原计划的只读 PCI 阶段**：`pcied` 消费 `PlatformPciTopology` root facts，做 intersection 并通过 kernel-minted、owner-bound
   `PciFabricAdmin` 扫描；BDF 取得 identity-only `PciFunction` capability。没有 config-write、BAR、MSI、bus-master 或 driver
   DMA authority。
6. **原计划的后续平台/PCI 管理**：先定义 `PlatformPciTopology` 与 root intersection，再逐项评审 `_OSC` ownership、PCI config write、
   BAR allocation、`DmaDomain`/IOMMU、MSI/MSI-X、IRQ、S5、EC、hotplug 和 suspend；每项均需 ResourceLease rights、故障/
   取消语义、QEMU 与硬件验证。

任何阶段都不得以“临时兼容”为由加入全局 `read_phys`、全 I/O port capability、服务专属 bootstrap hardware handle，或
允许普通 client 调用任意 AML path。

## 13. PRD 合入前不可豁免的设计门槛

| ID | 门槛 | 合入所需的规范性证据 |
| --- | --- | --- |
| G1 | SystemMemory page-cover | logical range、page envelope、safe direct-map-or-reject 判定、offset bounds 和 revoke 规则 |
| G2 | AML region dispatch | pinned `rust-acpi-naos` revision；三种 built-in region 均改为 token-relative fallible access |
| G3 | 执行资源上限 | `ExecutionBudget` 的 per-method 与 high-level work-unit fuel/deadline/depth/loop/allocation 合同；持久与临时 heap 预留、释放及 exhaustion outcome 均有测试 |
| G4 | `_STA/_CRS/_PRT` | 入口 allowlist、硬件 side-effect profile、串行顺序；root bridge 仅在匹配的 `_STA` snapshot 报告 present/functioning 时发布；SystemIO 需同 parent 固定 `_CRS` containment；不得标记为只读 |
| G5 | Firmware graph | canonical root、typed FADT edges、opaque object ID pointer resolution |
| G6 | AML load order | DSDT-first 与有序 SSDT `aml_load_ordinal` 合同 |
| G7 | SCI lifecycle | mask -> drain -> ack(seq) -> recheck/resignal-or-unmask 状态机、owner-bound `report_sci_failure`、crash/overflow 行为 |
| G8 | fixed ACPI hardware | `AcpiFixedHardware` inventory、generic-region deny 和 sealed sleep/reset handoff |
| G9 | FACS/Global Lock | FACS envelope reservation；kernel-private atomic Global Lock 与 PM1 GBL event 协议；AML `_GLK`/`Acquire`/`Release`/locked FieldUnit；QEMU HID/CID/UID discovery；QEMU 置 PM1 `GBL_STS` 并经 SCI 唤醒真实阻塞 waiter，再由 `platformd` drain/clear/ack |
| G10 | physical ownership | kernel-owned `PhysicalResourceRoot`/`ResourceLease` ledger 与原子 overlap/map/revoke transaction |
| G11 | DMA/MSI | Intel VT-d 与 AMD-Vi 均接入 vendor-neutral backend contract；两者的 default-deny、DMA allow/deny/fault、invalidation、interrupt-remapping/IRTE、owner teardown 与 unknown-outcome quarantine 有直接测试；Intel 与 AMD QEMU 核心资格验证通过即可，不要求真实硬件测试 |
| G12 | identity lifetime | opaque `firmware_version` 字符串覆盖 boot/firmware snapshot 身份，namespace/topology/fabric generation 分别拒绝 ABA |
| G13 | PCI discovery split | read-only `PciDiscoveryFabric`，仅 root intersection 后产生 `PciFabricAdmin` |
| G14 | `_OSC` | per-root query/commit、完整 evidence validation；denied/zero ownership 不创建 admin capability |
| G15 | compatibility profile | immutable `_OSI/_OS/_REV/_PIC` profile，且 `_PIC` 在 `_PRT` 前恰好执行一次 |

以下实现进度注记按日期追加，记录各自当时的状态；后续注记会覆盖较早的“仍未满足”结论。当前尚未关闭的门槛为 G2（fork revision
尚未固定到可复现的 fork gitlink）。G9 的 PM1 `GBL_STS`→SCI→waiter wake/owner ack QEMU 联合门槛，以及 G11 的 shared backend
conformance、AMD memory-IOPF delivery、requester BME disable 与 owner teardown QEMU 核心门槛均已通过。真实硬件测试不是验收前置条件。

实现进度注记（2026-10-19）：只读 `Namespace::get/traverse` 已改为共享借用；`platformd` 中无需执行 AML 的查询不再克隆
整棵 namespace。`snapshot_devices` 必需的隔离副本会先按 B-tree 上界申请 work-unit 临时 heap reservation，并在副本销毁后释放。
AML namespace 插入 hook 通过 `AmlName` 组件无分配计算路径长度，避免先分配临时 `String` 再向 meter 计费。`_CRS` parser scratch
会在解析前临时预留；AML 字符串字面量也会在构造前预留其 heap payload。规范化后的 cache data/key 会在构造/插入前预留为
持久 heap；cache key 字符串用有界 `try_reserve_exact` 复制，cache data 用共享 `Arc` 避免重复深拷贝。上述措施
还包含有界 device-candidate vector 和 snapshot 输出/缓存复制的 work-unit 预留；AML in-flight 参数向量、块栈增长及
DSDT/SSDT、method、嵌套 block 的字节码副本也在分配前预留，并使用 fallible exact reserve。namespace traversal 与 AML block
nesting 均限制为 256 层。AML Package、VarPackage、Buffer、ConcatRes 与大 Field 输出在 heap 预留后改为 fallible exact
allocation；`CopyObject`/direct `Store` 克隆动态 Object，以及 implicit `Store` 的 Buffer/String cast output 也经过预算和
fallible copy。`ToString`/`Mid` 的 String 输出现在先计入 heap reservation，再使用 fallible exact allocation；
`ToDecimalString`/`ToHexString` 的 integer/string/buffer 转换也先计算容量并预留 heap，再以单次 fallible allocation
直接格式化，避免逐字节临时字符串。`Mid` 的 index/length 运算不会溢出，切到 UTF-8 code point 中间会作为 AML 错误返回
而不是触发 panic。`Concat` 的 buffer/string 分支现在先 reservation 再单次 fallible allocation，直接写入最终输出；
`Buffer -> String` 保持 lossy UTF-8 语义，`NamePath` 不再创建中间 path String。AML String 到 Integer 的十六进制解析现在
使用借用输入，allocator 计数回归确认转换本身零分配。临时 heap RAII reservation 绑定单调递增的 work-unit generation，
旧 work-unit 的延迟 drop/commit 不会释放或提交新 work-unit 的 heap；对应 stale-reservation 边界有负向测试。
namespace object/package/buffer 的多维预算预留现在先校验全部计数与 heap limit，再一次性提交状态；拒绝的预留不再污染后续
work-unit 额度，覆盖了逐维度失败后仍可在剩余额度内成功的回归。`_CRS` 归一化输出向量改用 fallible exact reserve，
cache 数据只在成功插入时才把 working heap 转为 persistent；解析、分配失败或并发 cache 命中会释放临时 reservation。
BufferField 转整数现在直接在固定宽度栈缓冲区中读取最多 32/64 bit，不再先复制整个字段；运行时完整读取 BufferField
则先通过 execution meter 预留 `max_buffer_bytes`，并使用 fallible exact allocation，失败时回滚该 buffer reservation。
集成回归确认 4-byte 限额会在构造 1-Mibit/128-KiB 字段结果前拒绝执行。
NaOS 默认 `MimallocAllocator` 直接调用 mimalloc，不读取 quota、不写 quota header。AML 的 quota 由独立的
`HeapQuotaAllocator` 实例负责：`HeapQuotaScope` 仅提供当前 executor 的 allocator 工厂上下文，AML fork 的 `Vec`、`Arc`、`Box`、
`BTreeMap` 和自有 UTF-8 字符串类型显式保存 allocator。allocator 按 payload、allocation header 和对齐 padding 计费，charge 保留到实际
deallocation；扩容按新旧完整 charge 的差额预留/释放，后端失败时回滚。超过 `MEMORY_MAP_MAX_BYTES` 的布局会在调用 mimalloc 前拒绝。
旧版 global allocator quota 的 runtime 单元测试 17/17 通过、pinned NaOS release 构建通过；headless QEMU opt-in allocator-quota 测试确认跟踪、
超限拒绝、scope 外释放及 capacity 归还并以 pass marker 结束。ACPI fork host integration test 在真实 AML Method 的 BufferField 输出
路径注入 128-KiB allocator failure，并观察到无需堆分配的 `AmlError::BudgetExceeded`；meter 对 `LoadError::BudgetExceeded` 的静态映射
也有回归测试。新增的 target-only quota smoke 通过真实 `NaosExecutionMeter::begin_work_unit()` 安装 `HeapQuotaScope`，加载并执行
AML `MAIN`；测试将逻辑 buffer/heap budget 设为 8/32 MiB，并仅将真实 allocator quota 注入为 1 MiB，确认 4 MiB BufferField
先通过 meter 预留、再由 allocator 拒绝并返回 `BudgetExceeded`。通过 `tests/rust_allocator_quota_init.sh` 在 2048 MiB
headless QEMU 中 opt-in 运行，pass marker 与正常启动/shell marker 均通过。
上述端到端 OOM 测试验证的是旧版 global allocator quota；allocator 改为显式传递后，该 smoke 和 pinned NaOS 构建需要重新运行确认。
platformd 的 `_OSC` root path 入队副本改为最多
255-byte 固定尺寸消息字段，避免在 executor work unit 外分配第二个路径 String。G3 仍未满足：work-unit 外或跨线程 executor
scratch 及所有分配路径是否纳入同一上限，尚未逐项审计和证明。
2026-10-21：一次 `drain_and_rearm_sci`（包括最多八轮 coalesced sequence）现在由 worker-owned `SciWorkUnit` 包住；各 GPE
resolve/execute 请求加入同一 `ExecutionState` 与 `NativeAccessBudget` operation，而不是逐 handler 重置 opcode、loop、native-access
和 deadline。Buffer/scratch reservation 仍按本次调用释放，allocator quota 仍以真实 live allocation 记账。`gpe_methods_share_the_aggregate_sci_opcode_budget`
以真实 AML `_GPE._L2A/_L2B` 验证首个 handler 成功、共享额度耗尽后第二个返回 `BudgetExceeded`，结束 scope 后新 work unit 恢复额度；
`rust-acpi-naos` host tests 111/111 通过，pinned NaOS release build 与 2048 MiB normal boot 通过。该次历史 QEMU boot 没有产生 SCI/GPE event；
SCI event dispatch 后于 2026-10-25 在正式 512 MiB GPE.2 smoke 中通过。G3 当时仍未满足：work-unit 外或跨线程 executor scratch 及所有分配路径是否纳入同一上限，尚未逐项
审计和证明。
`platformd` 的 FirmwareRoot object/relation 分页聚合现在会在追加前按 kernel catalog 上限作 fallible capacity reservation，超限或
容量计算失败时拒绝该 catalog page；这只约束 catalog 输入聚合。AML/executor 的 allocator quota 与剩余缺口见下述 G3 记录。
FirmwareObject descriptor 的 `length` 现在保留到 AML adapter：在 open table 前验证每张 DSDT/SSDT 为 36 B–1 MiB 且总 AML
catalog 不超过 16 MiB；映射长度必须与 descriptor 相符，表字节复制及三组中间 vector 均使用 fallible reservation。目录验证的
固定序号 scratch 改用有界栈数组，root identity 改为标量扫描。这降低了启动期 firmware input 的 panic/OOM 面积。NaOS target
上的真实 allocator exhaustion outcome 已由 quota+AML smoke 覆盖，但 work-unit 外执行和 executor scratch 分配的完整边界仍未证明，
因此 G3 尚不满足。设备 HID/CID/UID 输出与短字符串副本现在使用 fallible exact
reserve；snapshot 将该分配失败映射为 `BudgetExceeded`，不再把内存耗尽误报为单设备 degraded。AML namespace 增加有界、
fallible `AmlName::try_to_string_bounded`；PCI root/preflight 与 device-function discovery 在创建路径字符串前检查编码长度并使用
可恢复分配。PCI device-function discovery 按 namespace 深度/设备数预留 path snapshot heap、复用有界 BDF path scratch；root
resource、routing 和 function path 的输出 vector 在 append 前检查上限并 fallible reserve。`pci_config_region_paths` 现在在遍历前
按 namespace 的 Device/PCI_Config parent path 计数与实际编码长度预留 work-unit heap；路径字符串/输出 vectors 使用 bounded
fallible reservation，祖先链用排序 path index 与复用的 16-entry scratch，不再为每个 region 做 unbounded `collect`。`snapshot_devices`
现在只创建一次 255-byte-bounded canonical path，并复用于 `_CRS` 查询与输出；缓存的 resource/address-space snapshot copy 使用
fallible exact reserve，失败映射为 `BudgetExceeded`，替代 `clone_from` 的隐式分配。`negotiate_pci_features` 现在在解析 root
path 前启动 work unit 并预留覆盖 AML path/query 参数的 scratch；query/commit 参数 vectors 使用 fallible exact reserve，路径
副本使用 `AmlName::try_clone`，错误分类直接检查既有错误消息，不再为 `Debug` 字符串复制分配。边界预算测试与 Q35 query/commit
启动 smoke 已通过。native `_REG` activation 现在先统计 scope/space 数量与路径大小，再为注册项、路径、Arc registry 和最大回调 scratch
预留 work-unit heap；路径复制、注册向量与 `_REG` 参数向量都使用 fallible allocation。注册以不可变 Arc 快照共享给回调，连接前缀只在每次
`_REG(space, 1)` 调用前推进，失败时按逆序回调 `_REG(space, 0)`；callbacks 不在 registry spinlock 内执行，teardown 在通知后释放
持久 heap 记账。只把已连接的受支持 space 判为可访问。`_REG` path 构造使用 fallible child append，避免隐式 `AmlName::resolve` clone；
GPE handler path 改为从固定 NameSeg fallibly 构造，不再在 work-unit 外 `format!`；combined dispatch 将查找和 AML 执行放在同一
work-unit，canonical namespace leaf lookup 不再为缺失 handler 分配诊断 path。Executor namespace traversal 现在用单个深度上限内的
可复用路径缓冲区，并在 metered 入口预留其 heap；reservation 由 RAII 在遍历成功或任何错误返回后释放，回归覆盖 callback/deadline
失败和预留拒绝。遍历按访问节点消耗 loop budget，每 256 个节点和遍历结束检查 work-unit deadline；
`_GLK` ancestor 检查直接沿组件链查找，不再每次 AML
调用都遍历整个 namespace 并格式化所有路径。Buffer 左操作数的逻辑比较现在对 String 右操作数借用数据，对需转换的 Buffer
先预留 `max_buffer_bytes` 与临时 heap、再进行 fallible exact copy，并在作用域退出时归还两项额度；回归覆盖预算拒绝、heap
拒绝回滚与正常 drop。OperationRegion token cache 现在在路径 key 构造前预留 working heap，在后端 `authorize` 前预留带 B-tree
节点/Arc 余量的持久 heap，并将缓存限制为 4096 项；失效逐 token 进行，不再复制全表临时向量，成功释放后归还对应计数，
释放失败保留 token/额度供重试。对应测试确认低 heap 时 backend authorize 不会被调用、失效恢复额度、释放失败仍保留额度。
platformd 与 ACPI worker 之间的 PCI root/device 快照现在以共享 `Arc` 跨线程传递，不再在 worker admission 前复制 root vectors
或每个 device 的 AML path；executor 直接校验并读取已有 snapshot，并在有界 work unit 内记账输出与 namespace scratch。
Q35 MSI-X route 与 multi-driver DMA/IRQ smoke 均在该镜像通过。2026-10-20：per-work-unit global allocator quota 已接入并通过 headless
QEMU opt-in quota smoke；全量 Rust smoke 随后在只读 boot-image-backed root 上的 `std::fs::create_dir` 收到 I/O error，该失败不证明
quota 失败，完整 filesystem smoke 仍未通过。后续增加的 ACPI fork host integration failure-injection 与 meter mapping 回归均通过，
NaOS 上真实 quota scope + AML executor 的端到端 OOM smoke 也已通过；`_OSC` 请求路径副本已改为固定尺寸存储。G3 仍要求其它
work-unit 外/跨线程 executor scratch 的完整边界审计。
2026-10-21：`ExecutorWorker` 加载固件表时不再在 work unit 外 `collect()` 构造 SSDT slice-reference vector；新增的 owned-table
backend API 直接遍历已限额的 `Vec<Vec<u8>>`，并保留 DSDT+SSDT 共用 operation opcode fuel 的回归。该修复关闭了一条有界但
未进入 executor allocator quota 的临时分配；其它 worker/executor 外部 scratch 尚待逐项审计，因此 G3 仍部分完成。
2026-10-21 更新：固定 AML namespace 的 10 个预定义对象及保守 heap 上界在创建解释器前计入预算；单个初始化 work unit 从
`Interpreter::new` 前覆盖 profile 安装、DSDT/SSDT、`_PIC` 封装和可失败的 metered Global Lock namespace 扫描。NameString 的
Root/ParentPrefix/NameSeg/MultiNamePath 组件数限制为 257，组件增长前预留；解析、父级搜索按 loop/deadline 计费，scratch 由 RAII
归还。AML 执行器的直接 `AmlName::resolve` 路径、namespace 搜索与动态 String-to-NamePath 解析现接入同一 meter，并在分配前
预留有界 heap；metered search/normalization 的路径诊断错误现改为不携带堆分配路径。`MethodContext` 的 table/method/block 字节流、活动 scope 路径及
block/in-flight/argument/scope vectors 均按真实生命周期持有 working-heap reservation；初始 in-flight 参数先 reserve 再构造。
嵌套方法帧按 context-stack 容量计入临时 heap。`_REG` 激活/回滚和 deactivation 的 callback scratch 由 RAII 归还；转为持久的
registration/path heap 只提交对应部分。`OpRegion.parent_device_path` 在创建时先为 clone 预留 working heap，只有 namespace 实际
接纳该对象后才转为持久记账。namespace 初始化与 `snapshot_devices` 不再克隆整棵 B-tree，而是先预留并 fallible-copy 有界
device-like path candidates，再在单一 interpreter ordering domain 逐个执行 AML；snapshot 的 `_STA/_INI/_HID/_CID/_UID/_PRT`
相对路径也走 metered entry。回归通过：ACPI fork library tests 83/83、`rust-acpi-naos` tests 117/117、NaOS Rust workspace、完整
`cmake --build build-acpi-pci -j2`；2048 MiB headless Q35 boot/EDU smoke 的启动 marker、child grant、BAR/config 权限、DMA allow/deny、
BME、fault source、MSI ack、teardown 与 VT-d trace 均通过。静态 `_S5` 查询现开启独立 work unit，并在解析固定路径前预留 heap；
缺失值用不复制诊断路径的 namespace 查询，预算不足后再次调用仍能正常开启 work unit。`_CRS` resource-template parser 的所有
descriptor（包括被跳过的 unsupported 项）现逐项扣 loop fuel、每 256 项检查 deadline；每次解析最多发出一条 unsupported-resource
warning。firmware `_CRS` 与 PCI link route 两条生产调用路径均使用 metered parser，预算/deadline outcome 不再被降格为普通 Invalid CRS。
以上均有回归覆盖。`_PIC` 失败时在最小 heap 预算下仍能完成 `_REG` deactivation 的负向回归通过。fork workspace 的 `bank_fields`
集成测试仍无法运行：ASL 编译阶段返回 `No such file or directory`，本机未发现 `iasl`；fork library tests 可单独运行。
2026-10-22：namespace heap 上界预扫描、PCI path heapsort/去重/root 选择/祖先二分查找、设备候选后处理现均按 loop budget 计费并
周期检查 deadline；新增 metered ancestor-search 与排序回归。region token 显式 invalidate 现在逐 token 计 loop fuel、检查 deadline，
并在预算/超时时保留尚未释放的 token 与 heap 记账。SCI 固定事件处理不再在 `platformd` caller thread 上分配事件或 pending-handler
vectors：用位迭代器惰性枚举最多 64+64 个 status bits，并以固定 128 项栈数组暂存 handler；稀疏、满位图和越界事件均有测试。
当前证据：ACPI fork library 86/86、`rust-acpi-naos` 123/123、platformd SCI 子集 7/7、Rust workspace、完整
`cmake --build build-acpi-pci -j2` 均通过；本次最新镜像的 2048 MiB headless Q35 EDU smoke 通过启动检查、child grant、BAR/config、DMA
allow/deny、VT-d faults、MSI ack、teardown 与 trace 检查。`NativeRegionAdapter::Drop` 的 best-effort token release 仍是最多 4096 项的
有界但未纳入 work-unit deadline/loop meter 的同步清理；此外 work-unit 外及跨线程 scratch 的完整边界仍未逐项审计和证明。因此 G3
仍部分完成，本文不能标记 Accepted。
2026-10-23：`_CRS` descriptor 归一化现对每项扣 loop fuel、每 64 项检查 deadline，并在完成后复核 deadline；固定 SystemIO
资源构造与 preflight 输出复制均改为 fallible、有界路径副本，预留 heap 上界包含每条最多 255-byte path，复制循环计入 work-unit
预算。`_CRS` cache descriptor 上限现在由 cache 内随插入维护的计数器检查，不再在每个 cache miss 时 fold 全表；插入处再次检查
容量以覆盖并发 miss，重复 key 不重复计数、缺失/无效 CRS 不增加 descriptor 数。新增 duplicate/overflow 回归。当前重新验证：
`rust-acpi-naos` 128/128、NaOS Rust workspace、完整 `cmake --build build-acpi-pci -j2`，以及最新镜像 2048 MiB headless Q35 EDU smoke
（启动、`_CRS` 设备资源、child grant、BAR/config、DMA allow/deny、VT-d faults、MSI ack、teardown 和 trace）均通过。G3 仍部分完成：
`NativeRegionAdapter::Drop`
的最多 4096 项 best-effort token release 尚未纳入 work-unit deadline/loop meter；work-unit 外及跨线程 scratch 的完整边界也仍需审计。
2026-10-23 续：继续 G3 审计后，批量 token teardown 一次 drain 到有界 scratch，再逐 token release；budget/deadline/release 失败时恢复未完成项，
避免逐项重扫 map。Native token 改为 inline capability 值并在授权前 `try_reserve` cache；`_CRS` cache 和 platformd 的 region/PCI path map
也在插入前采用 fallible capacity reservation。native backend 复用已计量的 parent path，资源代数查询前先完成 fallible path reservation，
native scalar read/write 前后检查 method/work-unit deadline。PCI namespace device-path heap 预扫描新增 metered 入口；`_PRT` 逐 route 扣
loop fuel、周期检查 deadline，重复 pin 检测改用固定 bitmap；SCI 整个 drain/worker IPC 受 10 秒总期限约束，超时继续 fail-closed 并保持
SCI masked。`NativeRegionAdapter::Drop` 不再同步执行 token-release RPC：正常清理由有预算的显式 deactivation 完成；清理失败时 platformd
退出并由 kernel 撤销 owner-scoped leases，Drop 只归还本地 heap accounting。fork 的 `ExecutionMeter` 新增 generation-scoped working-heap
reserve/commit/release；namespace traversal、NameString、temporary buffer 和 native token RAII guard 都把 originating generation 带回释放点。
work-unit 结束时仍未 drop 的 scratch 转入 stale-working heap 余额，继续占全局 heap 上限，直到对应 guard 归还；AML internal-method argument
vector 也改为 call-frame 生命周期的 RAII scratch。新增 cross-generation guard 和 stale heap admission 回归。
2026-10-23 续三：G3 动态值账本现区分 `WrappedObject` cell 与值 payload lease。解释器创建的 runtime cell（含 method Args/Locals 与内建 `_OSI`
结果）在 cell 分配前预留 working heap；String/Buffer/Package、NamePath、BufferField、转换及深拷贝输出在 fallible payload allocation 前预留
heap/package/buffer 多维额度。Store/CopyObject 替换 payload 时保留原 cell lease 并释放旧 payload；对象释放时按 originating generation 退回额度，
work-unit 结束时仍存活的对象继续计入 stale working set。namespace 插入会递归提交 Package/Reference/BufferField 所持的子对象 lease，带
深度上限和 ancestor-cycle 检查；未解析名称 String 也改为由实际 WrappedObject 持有 reservation。新增跨维度 reserve/commit/release、
clone/replace/drop、嵌套 Package 持久提交回归。当前验证为 fork library 93/93、`rust-acpi-naos` 133/133、Rust workspace、最新 pinned
NaOS Release 构建，以及 2048 MiB headless Q35 EDU smoke；后者检查 S18 `_CRS`、child grant、DMA allow/deny、两条 VT-d fault trace、MSI ack
和 teardown。`run.py` 按 30 秒超时停止 QEMU，test-specific wrapper 未报告缺失 marker 或 trace。
G3 的 dynamic Object/package/buffer 生命周期缺口现已闭合；G3 总门槛仍需完成 work-unit 外的 executor/worker scratch 与 allocator scope
覆盖范围的最终逐项核对，本文在该核对完成前不标记整体 Accepted。该 fixture 没有产生运行时 SCI/GPE event，因此这次启动验证不覆盖
SCI payload dispatch。
2026-10-24 G3 外围分配审计闭环：FirmwareRoot catalog 聚合和 AML 表副本有数量、单表与总字节上限并使用 fallible reservation；表副本
在 AML loader 中进入 package/heap 账本。executor loader 现在自身也拒绝超过 1024 张、单张 1 MiB、合计 16 MiB 的 AML 输入，直接调用库 API
不能绕过 platformd catalog 限制。executor worker 的请求队列、启动/setup channel 与单次回复 channel 均有固定容量；回复 payload 来自已
quota-scoped executor 的结果，并在实际释放前继续计入 allocator quota。SCI 暂存使用固定栈数组，最多八轮且总期限 10 秒。resource-attestation
的 wire buffer 有固定上界（最多 12 个 root resource windows），不进入 AML `max_heap_bytes`。loader 开始前的固定数量 meter/Arc 及这些
明确有界的 platformd protocol buffers 不属于 `max_heap_bytes`；该字段定义 AML/executor working heap 与 allocator quota，不是 platformd
全进程 RSS 上限。该边界已逐项审计，无剩余可达的无界外围堆分配。新增 loader 直入上限回归后，`rust-acpi-naos` 134/134、Rust host
workspace、pinned NaOS Release 构建，以及 2048 MiB headless Q35 EDU 启动和 test-specific wrapper 的 marker/VT-d trace 核验均通过。至此
G3 满足本门槛；Q35 CPU-hotplug GPE.2 的 SCI payload dispatch 与 matched acknowledgement 另有 2048 MiB 历史证据及 2026-10-25 的
512 MiB 复验，见 G7 记录。
以下 G9 注记记录早期的 fail-closed 阶段，现已被后续 Global Lock 实现取代，不再是当前验收结论。早期阶段的直接证据包括：FirmwareRoot 测试校验 FACS 地址/长度并拒绝小于 64-byte 的无效 envelope；kernel
ledger 测试确认 FACS `firmware_state` 的整页 cover 会阻止重叠 SystemMemory mapping，并通过启动路径使用的
同一 reservation helper 验证既有重叠 lease、非法页几何与页范围舍入溢出均拒绝；另一回归拒绝 FADT FACS
指针 alias 到既有 APIC record；FACS 仍不可作为普通 firmware object 打开。AML 测试覆盖 `_GLK`、直接/alias `Acquire` 与
locked FieldUnit 的 `NOT_SUPPORTED_GLOBAL_LOCK`/设备降级；fork 测试验证直接 acquire/release 也 fail-closed，public constructor
不再接受 FACS mapping。platformd health 测试确认 namespace Global Lock dependency 会置 `HEALTH_DEGRADED`。这满足“不实现 FACS lock、但依赖必须
fail-closed”的第一阶段行为合同，不代表 writable FACS 或跨 OS Global Lock 实现。源码在 FACS reservation 失败时清空 snapshot/ledger
并于创建 capability 前返回。现在 production 使用 `reserve_facs_state_then` 将全部 FACS envelope admission 作为 capability 构造
continuation 的门槛；kernel 测试先成功预留一条 FACS 页、再在第二条注入重叠 MMIO lease，确认 gate 拒绝、回滚第一条 lease，且
FirmwareRoot/AcpiResourceAuthority 构造 continuation 未执行。新增默认关闭的
`NAOS_TEST_ACPI_FACS_RESERVATION_COLLISION` kernel/test option；启用时 Q35 CTest 在真实 `ACPI::init` 中将 FACS page envelope
预留为冲突 MMIO，并由 test-owned boot wrapper 断言 reservation 失败、FirmwareRoot/AcpiResourceAuthority/PciDiscoveryFabric/
AcpiFixedHardware 未发布且无 kernel fault。该负向 boot test 与默认 option 关闭后的正常 Q35 boot 均已通过，因此 G9 的 capability
publication fail-closed 路径有直接启动级证据。此处记录仅说明旧阶段的拒绝行为及 FACS 保留不变量；当时不支持的 AML lock 路径
现改由 typed kernel Global Lock backend 提供，不得再据此将 `_GLK` 设备标记 degraded 或跳过 HID/CID/UID。
2026-10-25 当前 Global Lock HID 发现验收：`q35_acpi_global_lock_hid_boot` 在 512 MiB Q35 通过。QEMU 实际启用 FACS Global Lock SCI
event；fixture 的 `_GLK=1` 设备 `_STA` 返回 present/functional，`_INI` 在 `_GLK` 隐式锁内再次执行 AML `Acquire/Release`，设备保持
`initialized=true degraded=false`，platform topology 为 `healthy`，snapshot 明确包含 `HID=NAOS0001`、`CID=ACME0001`、`UID=42`。
对应 FACS Pending/Owned 原子转换、AML identifier decoding 和 PlatformPciTopology identifier flattening 单测通过。这个 QEMU gate
验证无竞争和嵌套 acquire/release 的真实 kernel 路径；Global Lock contention、GBL_STS 唤醒、timeout、release-failure 与 owner-exit
尚未由该启动 fixture 触发，因此仍保留为 G9 的回归覆盖工作，不影响当前 HID/CID/UID 发现目标已通过 QEMU 的结论。

G8 fixed-register GAS 现在保留并验证 `BitOffset`/`AccessSize`，按对齐的 1/2/4/8-byte 标量宽度执行 typed I/O/MMIO；不支持的
bit offset、access width、错位地址及跨越 PM1 status/enable 边界的访问均 fail closed。Q35 GPE0 的 `BitWidth=128` 表示 16-byte
block，解析器先按 FADT extent 拆成独立 status/enable bank，再为各 bank 选合法标量宽度；对应 24-case/226-assertion 固件单测
通过。`boot_smoke_platform_power_reset` 在 128 MiB guest 曾因内核扩栈 OOM 未到达 hook，2048 MiB 与正式 512 MiB 配置下的 Q35
typed reset clean-exit 测试均通过；这些结果是虚拟平台证据，不替代真实硬件验证。

2026-10-24 G3 最终预算错误路径复核：AML Method body 使用 fallible copy；静态 Global Lock 扫描按 window/prefix 计费并检查
deadline；`_CRS` 解析的 descriptor、IRQ、GPIO pin 与 ResourceSource 扫描均纳入 work meter/deadline，输出向量、字符串和 pin
数组在分配前 fallible reserve。`NativeRegionAdapter` 将 work-unit/heap exhaustion 映射到无 payload 的 `BudgetExceeded`，将
native-access fuel 映射到独立无 payload 的 `NativeAccessBudgetExceeded`，deadline 也使用 typed、无分配错误；quota 耗尽时错误报告
不会再分配字符串。验证：Rust host workspace 138/138、ACPI fork library 99/99、allocator-failure 集成测试 7/7、pinned NaOS
Release `make-root-image` 构建通过；2048 MiB headless Q35 allocator-quota smoke 和 production `virtioblkd` terminal DMA-fault
smoke 均通过。DMA-fault smoke 检查 EIO、child 非零退出、catalog removal、36 个 DMA mapping 清理及 MSI route teardown。该证据
完成 G3 本地代码与验证门槛；该证据不覆盖 G11 尚未完成的 AMD QEMU fault-event/owner-teardown 路径。

2026-10-25 正式 `-m 512` 复验：pinned NaOS Release `make-root-image` 后，独立 fault-disabled build 的
`q35_virtio_block_msix_io_boot` 通过 write/flush/read-back；fault-enabled build 的 `q35_virtio_block_terminal_dma_fault_boot`
通过，serial evidence 确认 BlockDevice EIO、child exit、provider/catalog removal、36 个 DMA mappings 清理和 MSI route teardown；
独立 allocator-quota init-hook boot 也通过 quota pass marker 与正常 shell startup 检查。三者的 serial marker 由
`tests/boot_smoke.py` 检查；通用 `util/run.py` 只管理镜像、QEMU 启动和生命周期。它们关闭此前记录的 512 MiB 软件回归缺口，
不替代 G11 端到端 AMD QEMU fault-event/owner-teardown 资格。

2026-10-25 G1 复核：kernel ledger 要求 `page_base == align_down(base)` 且 `page_end == align_up(base + length)`，拒绝前后超宽、
不足及溢出的 page cover；物理资源/FACS CTest 已覆盖精确正例和这些负例，新增 FACS reservation 上限回归确保超过 16 项时先回滚
已登记 lease、再拒绝 FirmwareRoot/Executor 初始化。AMD/通用 DMA 与物理资源/FACS 组合 CTest 106/106 通过；最新 Release
`make-root-image` 后 `q35_pci_discovery_boot` 以正式 512 MiB 配置通过，serial 到达 `pcied: INFO: ready regions=1 roots=1 devices=6`，
没有 kernel panic/fault。Rust `ResourceLease::new` 也要求非-I/O lease 使用精确 page cover；`rust-acpi-naos` host tests 139/139 通过，
新增回归拒绝超宽前后边界、不足 cover 和地址舍入溢出。该修改后的 NaOS Release `make-root-image` 与 512 MiB `q35_pci_discovery_boot`
均通过。FACS transaction stack scratch 从 511 个 ID（约 4 KiB）降为 16 项。

2026-10-25 HID/Global Lock 与协商复验：`PlatformPciTopology` 预发布 schema revision 15 的 `list_devices` 曾通过 kind 3 发布 `_UID`；上线前协议版本重置为 1，当前 `list_devices` 以方法级 `firmware_version` 字符串发布 snapshot identity。HID/CID
继续以独立 kind 发布；`pcied` 验证 UID 编码但只将 HID/CID 用于 driver matching。host 回归 platformd 58/58、pcied 34/34、
naos-idl 32/32、rust-acpi-naos 140/140、ACPI fork 107/107 通过。`_OSC` 接受 capabilities-mask 后的合法 support 子集，要求
commit support 与最终 query support 相等；越界位、query/commit 不匹配、错误及任何控制 grant 仍拒绝。原始解释器的顶层
`evaluate()` 与 AML 内部调用共用 `_PRT`/`_PIC` 顺序门禁。pinned NaOS Release `make-root-image` 通过；512 MiB QEMU 的
`q35_pci_discovery_boot`、`q35_acpi_sci_gpe_hotplug_boot`、`q35_acpi_global_lock_hid_boot`、`boot_smoke_platform_power_reset`、
`q35_amd_vi_boot`、`q35_pci_amd_vi_dma_msi_fault_trace_boot` 均通过。Global Lock QEMU fixture 验证无竞争与嵌套 acquire/release，
不覆盖 GBL_STS waiter wake、timeout、release failure 或 owner-exit；AMD EDU trace 中 QEMU 11.1.1 报告 IOPF.I=1，按 AMD 规范过滤后
不能作为 guest fault-source delivery/owner teardown 证据。AMD IOMMU 规范定义 IO_PAGE_FAULT event 字段；Linux AMD 驱动将
`I=0` 判为 memory transaction 并据此报告 DMA fault，而 QEMU 的 `amdvi_page_fault` 会无条件置 `IOPF_I`，因此其 trace
不计作 DMA fault-source delivery 资格证据；官方 QEMU 11.1.2 source 仍在 `amdvi_page_fault` 无条件置该位，故升级当前 11.1.1
测试机版本也不能提供合规 DMA IOPF 证据（[AMD IOMMU Specification](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/specifications/48882_IOMMU.pdf)、
[Linux AMD IOMMU driver](https://code.googlesource.com/linux/torvalds/linux/+/78273df7f646f8daf2604ec714bea0897cd03aae/drivers/iommu/amd/iommu.c)、
[QEMU AMD IOMMU 11.1.2 source](https://github.com/qemu/qemu/blob/v11.1.2/hw/i386/amd_iommu.c)）。G2、G9 与 G11 仍未关闭；`arch::ACPI` 仍直接包含 vendor controller
实现，G11 的 vendor-neutral adapter 边界也需继续收敛。submodule fork gitlink 尚未固定；本轮遵守不提交要求，未创建 submodule 或
父仓库提交。

同日 ACPI capability 原子发布负向复验：独立 `build-acpi-rollback` 使用
`NAOS_TEST_ACPI_INIT_CAPABILITY_ROLLBACK=ON`，在 FirmwareRoot、AcpiFixedHardware、AcpiResourceAuthority 与
PciDiscoveryFabric 私有 handle 均已构造、但 IOMMU 初始化前注入失败。512 MiB `acpi_init_capability_rollback_boot` 通过：
kernel 清理初始化事务，FirmwareRoot/FixedHardware/Executor/Discovery 均未发布且系统正常启动。该测试宏仅存在于该 opt-in build，
不影响正式 `build-debug`。

同日 Global Lock waiter wake/ack 复验：独立 opt-in `build-acpi-rollback` 开启
`NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE=ON`，测试 actor 先在 FACS 中持有 Global Lock；platformd AML worker 的 acquire 设置 Pending
并阻塞后，定时 callback 模拟固件 release，调用固定硬件的 SCI handler 路径唤醒 waiter。SCI owner claim 后，测试再注入合成
GBL_STS 事件，平台服务通过 typed drain/clear/ack 完成确认。512 MiB `q35_acpi_global_lock_waiter_wake_boot` 通过，日志确认
waiter wake、`PM1 Global Lock status acknowledged` 与 `_GLK` 设备 healthy。timer callback 和合成状态只在 opt-in test build 编译，
不伪称验证该 Global Lock 事件的物理 IOAPIC 投递。

同日 Global Lock timeout 复验：首次 50 ms QEMU 探针暴露 wait 唤醒后未复核 deadline、直接重试 FACS acquisition 的竞态；
`fixed_hardware::acquire_global_lock` 现于唤醒后、重试前检查 deadline，并在超时路径释放本地 ownership/FACS Pending。
512 MiB `q35_acpi_global_lock_waiter_wake_boot` 重建并通过（CTest 1/1）：serial 确认 acquire 返回 timed-out，后续真实
`_GLK` AML waiter 仍收到合成固件 release，SCI owner 清除并确认 GBL_STS，`PlatformPciTopology` 正常发布且设备 healthy。

同日 Global Lock owner-exit 与 release-failure 复验：独立 512 MiB opt-in QEMU build 分别通过
`q35_acpi_global_lock_owner_exit_boot` 与 `q35_acpi_global_lock_release_failure_boot`（各 CTest 1/1）。owner-exit 场景中
platformd AML worker 经 kernel executor 实际取得 FACS Global Lock 后退出整个进程；kernel owner cleanup 读回 FACS，确认
Owned/Pending 均清零。release-failure 场景在 `release_global_lock` backend 的实际调用路径注入一次 FACS release 失败；请求端
收到 `GlobalLockIoError`，后续 acquire 收到 `GlobalLockUnsupported`，并由 kernel serial 确认 Global Lock 状态进入 sticky quarantine。
故 G9 的 release-failure 与 process owner-exit 运行时门槛现已覆盖。512 MiB 正式宏关闭配置随后重跑
`q35_acpi_sci_gpe_hotplug_boot`（CTest 1/1）：QMP CPU hotplug 产生真实 Q35 GPE.2/IOAPIC SCI，serial 观察到
`pm1=1 gpe0=4`、`_E02` handler 完成和匹配 `SCI acknowledgement sequence=1 state=0`，证明实际 SCI route 与
mask/drain/ack/rearm 生命周期。Global Lock waiter wake 仍以合成 GBL_STS 状态调用 handler；当前剩余 G9 仅是 QEMU 中真实
PM1 GBL_STS 来源触发 SCI 并唤醒 Global Lock waiter 的联合覆盖，不能把 GPE.2 事件冒充为 GBL_STS。

同日 AMD EDU QEMU gate 复验：`build-debug` 的 `q35_pci_amd_vi_dma_msi_fault_trace_boot` 通过的是 25 秒限时内的启动、DMA deny
trace、成功映射传输和 MSI markers；serial 的 event-log 首记录为 `first_interrupt=true`，没有
`QEMU EDU IOMMU fault delivered` 或 domain/owner teardown marker，QEMU runner 到达时限。该绿灯只能证明 DMA 拒绝 trace，不能计作
AMD memory-IOPF fault-source 或 owner-teardown 资格；G11 继续保持未完成。

同日 G11 公共 fault dispatch 收敛：新增 `iommu::dma_fault_event` 与 `dispatch_dma_fault_events`，Intel VT-d 和 AMD-Vi 的
已解码记录均使用相同的 unit/requester 精确匹配、fault-source queue 投递和单次 overflow quarantine 策略；AMD 自己仍负责按
AMD 规范区分 memory/interrupt IOPF、拒绝非法 translation event。公共 dispatcher C++ 回归通过，覆盖跨 unit/requester 隔离、读写方向、
raw reason 保留及队列溢出只 quarantine 一次；宏关闭正式 kernel 构建通过。该改动统一了公共 fault delivery policy，但
`arch::ACPI` 仍持有 Intel/AMD controller、register callback 与 inventory 初始化，vendor-neutral controller adapter 抽离仍未完成，
且 AMD 真实 QEMU fault-source/owner-teardown 证据仍缺，不能据此关闭 G11。

该改动后的 Intel 端到端复验：512 MiB `q35_pci_msi_route_prepare_boot` CTest 1/1 通过；serial 确认 `PciDmaFaultSource` 收到未映射
IOVA fault、domain close 前的第二条 fault 可 drain、DMA domain 与 MSI route teardown 完成。这证明 Intel VT-d 的运行时 fault delivery
已经经过公共 dispatcher；它不补足 AMD event-source 行为或 `arch::ACPI` controller adapter 抽离。

同日 provider registry 所有权收敛：boot-scoped `iommu::provider_router` 状态从 `arch::ACPI` 全局变量移入
`naos/src/kernel/iommu/backend.cc`；操作表统一命名为覆盖 DMA、fault 与 interrupt 的 `iommu::provider_ops`，ACPI 启动和 PCI/IRQ
consumer 通过 `kernel_provider_router()` 访问公共接口。五项 provider-router C++ 回归和上述 Intel QEMU route/fault/teardown gate 均通过。
该调整只迁移 registry ownership 与收敛公共操作表命名；`arch::ACPI` 仍实现并
注册厂商 callback、持有 Intel/AMD controller 与 inventory-specific orchestration，不能宣称 8.5 的完整 backend adapter 边界已经完成。

同日 Global Lock PM1 释放通知错误路径修正：`signal_global_lock_release` 现在必须成功读取 PM1 control 才会设置 `GBL_RLS`，避免读取失败后把零值当作寄存器内容写回、覆盖其他 PM1 控制位；失败仍沿 Global Lock sticky quarantine 路径返回。PM1 control 位保留 C++ 回归通过，宏关闭 kernel build 通过，512 MiB `q35_acpi_global_lock_hid_boot` 复验通过，serial 确认 `_GLK` 设备 healthy 并发布 `HID=NAOS0001`、`CID=ACME0001`、`UID=42`。该修复不把 QEMU 合成 waiter event 记作真实 GBL_STS SCI 来源，G9 的该联合验收仍未关闭。

同日 QEMU G9 能力核查：官方 QEMU 11.1.2 `hw/acpi/core.c` 的 `acpi_pm_cnt_write` 只更新 PM1 control shadow 并处理 sleep request，没有实现 `GBL_RLS` 到 runtime-firmware release、PM1 `GBL_STS` 或 SCI 的硬件响应（[QEMU v11.1.2 ACPI core](https://github.com/qemu/qemu/blob/v11.1.2/hw/acpi/core.c)）。因此现有 stock QEMU 无法为真实 `GBL_RLS -> GBL_STS -> SCI` 提供端到端证据；不得用 NaOS 测试 callback 合成状态冒充该事件。HID/CID/UID 发现的 `_GLK` QEMU gate 已通过，但 G9 的协议事件源验收仍保持开放。

同日 G11 fault-source registry 收敛：provider/unit/requester 绑定、capability 内部引用、quarantine 查询和 queue/unit overflow 投递从 `arch::ACPI` 的 Intel/AMD 两份静态数组移入 `iommu::backend.cc` 的单一 kernel registry；AMD 仍在自己的 decoder 中按规范过滤 interrupt IOPF 和非法 translation event，Intel IRQ drain 与 AMD poll 均将规范化事件交给同一个 registry。回归验证同 requester 跨 provider 隔离、duplicate registration 拒绝、队列溢出只 quarantine 一次，以及后续 unit quarantine 仍处理已 quarantine requester。该改动后的宏关闭 kernel build、这些 C++ 回归、512 MiB Intel `q35_pci_msi_route_prepare_boot` 均通过；AMD `q35_amd_vi_boot` 与 trace-only `q35_pci_amd_vi_dma_msi_fault_trace_boot` 通过启动/默认拒绝/路由 markers，但 QEMU event 仍是 interrupt IOPF，未形成 AMD memory-fault delivery 或 owner teardown 证据。该 registry 抽离减少了 `arch::ACPI` 的 fault-source bookkeeping，但厂商 controller、register callback 与初始化 orchestration 仍未移入独立 adapter，G11 继续开放。

同日 G11 错误映射归属修正：VT-d `error` 与 AMD-Vi `controller_error` 到 kernel `na_status_t` 的映射现由各自 vendor backend 的 `to_status` 实现；`arch::ACPI` 不再维护两套厂商枚举转换表。逐项覆盖两个枚举的分类回归 2/2，VT-d/AMD-Vi IOMMU C++ suite 97/97、宏关闭 kernel build 均通过。该改动后 512 MiB Intel `q35_pci_msi_route_prepare_boot` 与 AMD `q35_pci_amd_vi_dma_msi_fault_trace_boot` 各 CTest 1/1 通过；AMD serial 仍只有 `first_interrupt=true` event marker，没有 guest DMA fault-source delivery 或 owner teardown marker。该步骤仍不移走 ACPI inventory 初始化、register callbacks 或 controller/domain operation callbacks，不能作为完整 provider adapter 边界的证据。

同日 AMD-Vi PCI capability 解析收敛：AMD backend 现在负责校验 IOMMU capability ID/type/reserved bits 与 PAsize 12–52；`arch::ACPI` 仅经其受 ledger 和 ECAM session 保护的回调读取 vendor/header/MiscInfo snapshot。capability offset 上限从 `0xff0` 收紧到 `0xfec`，确保 `MiscInfo` dword 完整位于 4 KiB PCI config space。新增有效边界、错 ID/type、reserved bit、PAsize 越界和读取失败 C++ 用例；AMD controller suite 35/35 通过，更新后 512 MiB AMD EDU trace QEMU CTest 1/1 通过，事件仍按规范识别为 interrupt IOPF，不能计作 memory-fault delivery。该解析移动没有转移 controller 初始化、register callbacks 或 DMA/route operation ownership，G11 仍开放。

本文整体只有在 G1–G15 全部具备对应 NaoIDL/Rust internal contract、负向行为测试和要求的 host/QEMU 验证后才能标记为
Accepted；真实硬件测试不是验收前置条件。G11 与 G14 属于 Phase 6 的 DMA/MSI 和 `_OSC` ownership 实现门槛；它们不是宣称 Phase 1–5
已实现的依据，也不能
因 Phase 1–5 完成而从整体 Accepted 条件中豁免。

同日 G11 中断路由 vendor ownership 收敛：Intel VT-d 与 AMD-Vi 后端现各自校验 requester/source generation、安装 MSI/IOAPIC
remapping route、编码 provider token、匹配 PCI source 并撤销 route；`arch::ACPI` 保留 snapshot generation、provider lock、日志和
公共 callback 适配，不再编码厂商 route/token 细节。宏关闭 kernel、Intel/AMD route controller tests 编译通过；VT-d/AMD-Vi
单测 102/102 通过，512 MiB `q35_pci_msi_route_prepare_boot` 与 `q35_pci_amd_vi_dma_msi_fault_trace_boot` QEMU gates 通过。
AMD trace 用例的运行时限从 25 秒调至 45 秒以容纳 trace 下完整 shell startup，保留原有 boot 和 trace assertions；serial 确认
`init: user shell started`、EDU DMA/MSI 与 unmapped IOVA request 完成。AMD event-log 首记录仍为 `first_interrupt=true`，没有
`QEMU EDU IOMMU fault delivered` 或 DMA-domain/owner teardown marker，不能据此关闭 AMD memory-IOPF delivery 或 teardown 门槛。
512 MiB `q35_acpi_global_lock_hid_boot` 同源复验通过，`_GLK` 设备 healthy 并发布 `HID=NAOS0001`、`CID=ACME0001`、`UID=42`。
该抽取减少 `arch::ACPI` 中的厂商 route 逻辑，但 controller、register callbacks、DMA callbacks 与 inventory 初始化仍未整体移入
独立 backend adapter；G11 继续开放。按用户要求，本轮未提交或暂存更改。

同日 G11 fault registry 锁边界修正：`dispatch_registered_dma_fault_events` 与
`quarantine_registered_dma_fault_sources` 现在只在 registry spinlock 内选取并复制 registration、持有 source capability 引用、
以及更新 quarantine 状态；队列投递、source close/overflow 通知和 PCI bus-master disable 均在释放 registry lock 后执行。
registration generation 校验防止旧事件在 unregister/re-register 后 quarantine 新 owner；unregister 先标记 retiring，完成 close 后才
允许复用槽位。unit quarantine 在本次启动内保持 sticky，并阻止该 provider/unit 注册新的 fault source；相同 unit index 的另一
provider 保持隔离。新增 callback 重入 registry 查询与 quarantine 可见性回归，并为该测试目标设置 10 秒超时。更新后 kernel build、
fault-dispatch 定向回归 4/4、VT-d/AMD-Vi suite 102/102 通过；512 MiB Intel `q35_pci_msi_route_prepare_boot` 与 AMD
`q35_pci_amd_vi_dma_msi_fault_trace_boot` 均通过。AMD 串口事件仍为 `first_interrupt=true`，本轮证据不包含 AMD guest
memory-fault delivery 或 domain/owner teardown；G11 adapter 抽离和这两项资格仍开放。

同日 G11 DMA page operation vendorization：VT-d 与 AMD-Vi 的 page map/unmap/domain destroy status 和
`outcome_uncertain` 推导现由各自 backend helper 执行；`arch::ACPI` DMA callbacks 保留 owner-bound state、provider lock、fault-source
registration 与 capability lifecycle。更新后 kernel、Intel/AMD controller tests 编译通过，VT-d/AMD-Vi C++ suite 102/102 和
512 MiB Intel route/fault/teardown、AMD EDU trace QEMU gates 均通过。该移动仍未迁移 DMA domain creation policy、controller instance、
register operations、fault drain loop 或 provider initialization；完整 G11 adapter contract 继续开放，AMD memory-IOPF delivery 与 owner
teardown 仍未由符合规范的 QEMU event 验证。

同日 G11 kernel register adapter 收敛：VT-d 的 DMAR aperture register access、DMA page allocation、physical mapping 与 cache flush，
以及 AMD-Vi 的 IVHD register access、contiguous buffer allocation、physical mapping 与 cache flush，现由各自
`*_kernel_adapter.cc` 提供 ops factory；`arch::ACPI` 只负责建立/撤销经 resource ledger 授权的 MMIO aperture，并将 AMD PCI
BME/requester 查询回调传给 AMD adapter。kernel（含两个 adapter translation unit）、Intel/AMD controller test targets 编译通过；
102 项 IOMMU suite 和 512 MiB Intel route/fault/teardown、AMD EDU trace QEMU gates 通过。`arch::ACPI` 仍持有 vendor controller
instance、DMA-domain creation、fault drain 与初始化编排，故 G11 的完整 adapter ownership 尚未完成；AMD QEMU event 仍是
`first_interrupt=true`，不计为 guest memory-fault delivery。

同日最新 kernel adapter 构建复验 HID 发现：512 MiB `q35_acpi_global_lock_hid_boot` CTest 1/1 通过；serial 确认
`\_SB_.GLKT` `initialized=true degraded=false`，`PlatformPciTopology` healthy 发布后 `pcied` 成功读取 topology page；设备标识为
`HID=NAOS0001`、`CID=ACME0001`、`UID=42`。这确认寄存器 ops 抽取没有破坏用户态 HID/CID/UID 发现路径。

同日 G11 controller state ownership 迁移：Intel VT-d 与 AMD-Vi 的 boot controller instance 及其 domain serialization lock
现存放于各自 kernel adapter translation unit；`arch::ACPI` 中的 controller 全局变量已移除。adapter 暂时仍公开 raw controller/lock
accessor，ACPI operation callbacks、DMA-domain creation、fault drain/scratch、AMD fault-poll lifecycle 和启动编排仍直接使用 vendor
API，因此这只是 ownership 移动阶段，不能视作 8.5 的 vendor-neutral adapter 边界完成。更新后 kernel/controller test targets 编译
通过，VT-d/AMD-Vi suite 102/102、Intel 与 AMD QEMU gates 均通过；AMD event 首记录仍为 `first_interrupt=true`，未验证 guest
memory-IOPF delivery 或 owner teardown。

同日 G11 controller 操作 facade 收敛：Intel/AMD raw `kernel_controller()` 与 `kernel_controller_lock()` 已从公共
backend header 和 ACPI 调用路径移除。各 vendor adapter 现在自行串行化初始化、状态/计数查询、route 匹配与撤销、DMA
domain create/map/unmap/destroy、故障记录 drain，以及 AMD unit quarantine；`arch::ACPI` 不再拿到 controller 或其锁。
宏关闭 kernel 和两个 controller test targets 编译通过，102 项 VT-d/AMD-Vi suite 通过；新鲜 `build-debug` userland/root image
上的 512 MiB QEMU HID gate 1/1、Intel 与 AMD route gates 2/2 通过。HID serial 确认 `_SB_.GLKT` healthy 并发布
`HID=NAOS0001`、`CID=ACME0001`、`UID=42`；Intel 确认两条 fault 的 delivery/drain 和 domain/MSI teardown。AMD serial 的
首个 event 仍为 `first_interrupt=true`，所以该 gate 只证明启动、DMA/MSI 与 event-log 路径，不证明 memory-IOPF delivery 或
domain/owner teardown。`arch::ACPI` 仍负责 provider/domain capability 生命周期、requester policy、PCI config 与 fault-poll
编排；G11 的完整 adapter 边界以及 G2/G9 仍开放。本轮没有暂存或提交。

同日 G11 AMD owner teardown 资格补测：AMD EDU QEMU serial 的 AMD-Vi event 是 `first_interrupt=true`，AMD decoder 按规范不把它
伪装成 memory IOPF。为使该 no-report 分支仍覆盖资源回收，opt-in EDU smoke 对 fault-source `take` 做 256 次有界 async turn；若无
fault record 或 source 返回错误，会保持 smoke 失败，同时执行 BME disable、DMA unmap、domain close 和 MSI source close。512 MiB
AMD QEMU CTest 1/1 通过，serial 显示缺少 fault record、BME disabled、page unmapped、domain closed、MSI route source closed，且
`pcied` 观察到 child 以失败状态退出。此证据验证 QEMU 上的 failure-path owner teardown，不验证 AMD memory-IOPF delivery；G11
仍开放，G2/G9 与 vendor-neutral adapter 全面收敛也仍未完成。

同日 G11 fault/event drain vendorization：Intel VT-d fault-record scratch、record-to-`dma_fault_event` normalization、fault overflow
投递现由 Intel adapter 负责；AMD-Vi event-log scratch、按 AMD 规范过滤/解码以及 unit quarantine 现由 AMD adapter 负责。两者
均向公共 IOMMU backend 注册 drain callback，并返回不暴露厂商 enum 的 `fault_drain_report`；ACPI fault report path 仅提供
PCI BME-disable callback、调用通用 provider drain 并调度 retry。adapter-owned scratch 使用非阻塞 single-flight lease，未在
controller/registry/BME callback 上持有 ACPI raw spinlock。AMD fault-poll stop 也经公共 provider dispatch 隔离所有 units。
kernel build、102 项 VT-d/AMD-Vi suite 和 512 MiB HID/Intel/AMD QEMU gates 3/3 通过。AMD event 仍为 `first_interrupt=true`；
QEMU no-report teardown gate 证明 BME-off/unmap/domain/MSI cleanup，不证明 memory-IOPF delivery。ACPI 中 requester policy、DMA
domain capability construction、provider initialization、route adaptation 和部分 vendor status handling 仍未全部移入 adapters；
故 G11 的 8.5 contract 仍未完成，G2/G9 继续开放。本轮没有暂存或提交。

同日 G11 fault-drain 锁边界修正：Intel/AMD fault report 的 `RawSpinLock` 不再跨越 controller drain、日志、fault registry
分发或 PCI BME callback。ACPI 现在用非阻塞的 single-flight buffer lease 保护共享 scratch；竞争中的报告调用立即请求重试，
不在 spinlock 上等待。AMD unit quarantine 标志改为 atomic。kernel/root image 构建通过；512 MiB HID、Intel fault/teardown 与 AMD
no-report teardown QEMU gates 3/3 通过，Intel 两条 fault batch 均 delivery/drain，AMD 首条 event 仍是 `first_interrupt=true`，
没有 AMD memory-IOPF delivery。共享 scratch、AMD decoder、vendor error/result 类型与 polling orchestration 仍留在
`arch::ACPI`；G11 vendor-neutral adapter 合同尚未完成，G2/G9 仍开放。

同日 G11 DMA-domain 私有生命周期迁移：新增 provider-neutral DMA adapter ops 注册与 dispatch。Intel VT-d/AMD-Vi adapter
现在各自私有持有 vendor domain state，负责 domain 创建、fault-source 注册/撤销、方向转换后的 DMA page map/unmap 与
hardware domain destroy；ACPI 不再声明 vendor domain 类型、不再直接调用 vendor domain map/unmap/destroy，也不再注册 fault
source。`PciDmaDomain` 的公共 wrapper 只保存 provider tag 与 opaque backend state，并把 DMA 操作交给通用 dispatch。ACPI
仍负责当前固件/拓扑授权、ECAM/BME 策略、AMD source 与能力验证，以及公共 capability 构造和 fault drain 编排，因此这还
不是完整 G11 adapter 抽离。adapter 改动后 kernel 编译通过，相关 ACPI/IOMMU/DMA CTest 126/126 通过；512 MiB QEMU 的
AMD-Vi 启动、Global Lock/HID/CID、Intel MSI route 与 AMD EDU DMA/MSI/no-report teardown gates 4/4 通过。HID serial 读到
`HID=NAOS0001`、`CID=ACME0001`、`UID=42`；AMD EDU domain 完成映射、DMA 和 BME-off/unmap/domain/MSI teardown，但 QEMU
未提供 AMD memory-IOPF record，故不能声称 fault delivery 已验证。G11 与 G2/G9 仍开放。本轮没有暂存或提交。

同日 G11 ACPI fault event API 再收敛：fault drain callback 由 Intel/AMD adapter 在各自初始化时注册到 kernel IOMMU owner；公共
`drain_provider_fault_events(provider, unit, quiesce_ops, report)` 只返回 normalized summary，adapter callback 在释放 provider
adapter registry lock 后执行。Intel raw fault record 转为通用 `dma_fault_event` 并通过公共 source registry 投递；AMD event scratch、
AMD 规范 decoder、unit quarantine 和 poll-stop unit quarantine 都在 AMD adapter。`arch::ACPI` 不再声明/持有厂商 fault scratch、
record/event drain result 或调用 AMD vendor decoder，只调公共 drain 并提供 ECAM BME-disable callback。该抽取后的 kernel、102 项
VT-d/AMD-Vi suite 和 512 MiB HID、Intel fault/teardown、AMD no-report teardown QEMU gates 均通过；AMD no-report 分支仍不计作
memory-IOPF delivery。DMA domain state/create/destroy、PCI requester policy、route callback 适配和 provider 初始化仍留在 ACPI，
故 G11、G2/G9 仍未关闭。本轮没有暂存或提交。

后续 G11 DMA-domain policy 再收敛：provider adapter `create` 现在回传 opaque backend state、硬件地址宽度与 opaque unit
index；ACPI 的公共 `create_pci_dma_domain` 只做 topology lineage、通用 BDF/source binding、ECAM lease 和 AMD fault-poll
启动门槛，不再读取 DMAR/IVRS unit opaque、检查 Intel fault IRQ、解析 AMD requester 或读取 AMD capability/PA width。
Intel adapter 在建域前校验本 unit fault interrupt 已启用；AMD adapter 自己通过当前 IVRS inventory 重解 requester、读取受
ECAM 授权的 IOMMU capability，并按 CPU/IOMMU 两侧宽度较小值建域。kernel build 与 126 项 ACPI/IOMMU/DMA CTest 通过。
QEMU 暴露并修复一个原有 teardown 错误：provider route 按协议在 drain 后先退休，随后恢复 MSI 配置的 validator 却仍要求
route live，导致恢复阶段误判并 quarantine。恢复现在复核当前 requester lineage 和 token generation；AMD QEMU smoke 新增
teardown-complete 必需 marker 与 quarantine 禁止 marker。512 MiB Intel EDU fault/teardown 与 AMD EDU no-report teardown
QEMU gates 均通过，serial 确认两家 `PCI MSI route teardown complete`。AMD event 仍为 `first_interrupt=true`，没有 memory-IOPF
delivery；ACPI 中 provider initialization、source/route callback 适配仍未完全移入 adapters，故 G11、G2/G9 仍开放。本轮未提交。

同日 AMD capability read 边界再收敛：AMD adapter 不再要求 ACPI 构造 AMD 专用 `pci_iommu_capability_snapshot`；它通过通用
PCI config dword callback 分别读取 vendor/device identity、IVHD 指定 capability header 与 `MiscInfo`，并在 adapter 内校验
capability ID/type/reserved bits 和 PA width。ECAM 地址解析、resource lease、config serialization 仍由 ACPI hardware adapter
实施。更新后的 AMD capability C++ 测试通过，512 MiB AMD EDU QEMU smoke 再次通过；serial 确认 DMA transfer、no-report teardown
和 `PCI MSI route teardown complete`，quarantine 禁止 marker 未触发。AMD memory-IOPF event 仍未由 QEMU 投递，G11 继续开放。

后续 G11 route callback ownership 迁移：Intel/AMD 的 MSI/IOAPIC route 安装、token match/retire 与 interrupt-remapping 状态
callback 及各自 `provider_ops` 构造现位于 vendor kernel adapter；ACPI 不再实现或直接调用这些 vendor route operation，注册时只
传 source resolver 与统一 DMA-domain factory。kernel build、126 项 IOMMU/DMA suite 通过；512 MiB Intel EDU fault/route teardown
和 AMD EDU DMA/MSI/no-report teardown QEMU gates 2/2 通过，serial 均显示 `PCI MSI route teardown complete`，AMD quarantine
禁止 marker 未触发。ACPI source resolver 仍直接解析 DMAR/IVRS，AMD IOAPIC safety inventory、provider initialize/registration 和
fault-poll lifecycle 编排也仍留在 ACPI；G11 adapter contract 未完成，AMD memory-IOPF delivery 与 G2/G9 仍开放。本轮未提交。

后续 source-resolution ownership 迁移：Intel adapter 现在持有 DMAR PCI bridge-reader 与 MADT IOAPIC match-count platform
callback，并在 adapter 内解析 PCI/IOAPIC source；AMD adapter 在自身 inventory/controller 下解析 PCI/IOAPIC source。ACPI
provider registration 不再传 vendor source-resolver 函数。AMD IOAPIC pass-through safety inventory 也改为 ACPI 仅传 MADT
enumeration IDs，AMD adapter 内部按 IVRS 解析、检查歧义并构造 controller source table；ACPI 不再分配/填充 AMD 专用
`ioapic_interrupt_source` 或 `activation_preconditions`。更新后的 kernel build、126 项 IOMMU/DMA suite 和 512 MiB Intel/AMD
EDU route/DMA QEMU gates 2/2 通过。ACPI 仍拥有 register-aperture resource lease、fault IRQ/timer wiring、provider initialize
调用/状态日志，以及 `start_deferred_services` 编排；这些部分和 AMD memory-IOPF QEMU delivery、G2/G9 仍开放。本轮未提交。

后续初始化结果边界收敛：Intel/AMD adapter 的 `kernel_initialize` 现返回 vendor-neutral
`provider_initialization_report`（status、state uncertainty 与 unit readiness counts）；ACPI 不再处理
`intel_vtd::error`/`amd_vi::controller_error` 或调用 `to_status` 转换。相关 kernel 编译、126 项 IOMMU/DMA CTest 和 512 MiB
Intel/AMD EDU QEMU gates 2/2 通过。ACPI 仍直接管理 VT-d fault IRQ vector/handler、AMD poll timer 生命周期和部分 backend readiness
查询；G11 未关闭，G2/G9 仍开放。本轮未提交。

后续 AMD fault-poll ownership 迁移：polling active state、timer scheduling、per-unit normalized fault drain 与 timer exhaustion
后的 provider quarantine 现由 AMD adapter 持有；ACPI 仅调用通用 `start_provider_fault_polling` 并提供 requester BME-disable
policy callback。DMA domain create 与 bus-master enable readiness 也经 provider adapter callback 检查 polling 状态，ACPI 不再
维护 AMD polling atomic、遍历 AMD unit count 或分发 AMD fault summary。kernel build、126 项 IOMMU/DMA CTest 和 512 MiB Intel/AMD
EDU fault/teardown QEMU gates 2/2 通过。ACPI 尚保留 PCI/AML topology 启动、register-aperture resource ledger/map、Intel fault IRQ
handler/vector 生命周期，以及 vendor adapter bootstrap 调用；G11 未完成，AMD memory-IOPF event、G2/G9 仍开放。本轮未提交。

后续 fault IRQ boundary 收敛：公共 `configure_provider_fault_interrupt(provider, unit, apic, vector, outcome)` dispatch
现由 Intel adapter 注册，并在内部处理 vendor fault-interrupt configure/status；ACPI 仅分配 vector/handler、做 IRQ registration
及保留 uncertain route 的生命周期。AMD poll loop 和 deferred startup 改用初始化 report 的 unit count/status，不再调用 AMD
controller readiness/count 查询。kernel build、126 项 IOMMU/DMA suite 与 512 MiB Intel fault/teardown、AMD DMA/no-report teardown
QEMU gates 2/2 通过。ACPI 剩余 vendor 调用集中于 bootstrap 入参/adapter ops 构造；启动 aperture mapping、provider registration
协调和 fault-poll timer ownership 尚需继续抽离。AMD memory-IOPF delivery、G2/G9 仍未验收，本轮未提交。

后续 ACPI provider lifecycle 再抽离：fault IRQ configuration 由 Intel adapter callback 经公共 fault dispatcher 执行；AMD adapter
接管 fault-poll active state、timer scheduling、normalized per-unit drain、timer failure quarantine。ACPI 只保留 IRQ vector/handler
注册与 BME-disable policy，并通过 `start_provider_fault_polling` 启动 AMD poll；domain create/BME admission 从通用 adapter readiness
callback 查询 polling 条件。两家 `kernel_initialize` 均返回 vendor-neutral `provider_initialization_report`，ACPI 不再解释 vendor
error enum 或直接查 unit readiness/count。kernel 编译、126 项 IOMMU/DMA CTest 和 512 MiB Intel/AMD EDU QEMU gates 2/2 通过；Intel
fault delivery/route teardown、AMD no-report owner teardown 与 route teardown 都有 serial 证据。剩余直接 vendor 接触只在 ACPI
bootstrap API/ops 构造；provider aperture lease/map 与 provider registration 仍需下沉，AMD memory-IOPF、G2/G9 仍开放。本轮未提交。

后续 provider bootstrap dispatch 收敛：ACPI 改为通过通用 `provider_bootstrap_config` 和
`initialize_provider_backend(provider, config)` 初始化 IOMMU，不再 include Intel VT-d/AMD-Vi header、构造 vendor controller ops、
调用 vendor `kernel_initialize/kernel_provider_ops`，也不再注册 provider ops；kernel IOMMU owner dispatch 到对应 adapter，adapter
内部初始化并注册自己的 provider callbacks。bootstrap config 携带 opaque firmware inventory、generic requester/PCI config、bridge
reader、MADT match counter、quiescence 与统一 domain factory。kernel build、126 项 CTest 和 512 MiB Intel/AMD EDU QEMU gates 2/2
通过；AMD fault polling 已由 AMD adapter 启动。ACPI 仍负责依据 DMAR/IVRS inventory 预留/映射 register apertures、PCI quiesce、
Intel IRQ vector/handler setup 和外层初始化顺序；G11 仍开放，AMD memory-IOPF、G2/G9 也未关闭。本轮未提交。

后续 G11 register-aperture ownership 收敛：bootstrap config 增加通用 register-aperture acquire/release callbacks；Intel adapter
根据已验证的 DMAR units、AMD adapter 根据选中的 IVHD records 申请并持有各自寄存器窗口，并在进入 vendor controller 前检查
PCI quiescence 及当前不支持的 RMRR/IVMD 条件。ACPI 只实现物理资源账本 reservation、kernel mapping 和释放 callback，不再
遍历任一厂商 inventory 来 reserve/map aperture。准备阶段部分失败会回滚此前窗口；进入控制器初始化后保留窗口，避免回收可能
仍被硬件使用的映射。kernel build、126 项 ACPI/IOMMU/DMA CTest 和 512 MiB Intel/AMD EDU QEMU gates 2/2 通过；Intel fault、DMA、
MSI route teardown markers 通过，AMD DMA/MSI/no-report teardown markers 通过且没有 quarantine。AMD QEMU event 仍是
`first_interrupt=true`，不能据此声称 memory-IOPF delivery 已验证。ACPI PCI quiesce、Intel fault IRQ 生命周期和初始化编排仍待
继续抽离；G11、G2/G9 仍开放。本轮未暂存或提交。

后续 provider-neutral fault delivery 编排收敛：初始化报告增加 `fault_delivery_mode`，Intel adapter 声明 interrupt、AMD adapter
声明 polling。ACPI 将原 VT-d 专属 IRQ slot/soft-drain 改为按 provider index 管理的通用 IRQ slot；BME-off 后是否记录 fault 与
是否重排 IRQ drain 也由 provider mode 决定。deferred startup 遍历已初始化 provider，只对声明 polling 的 backend 启动通用
`start_provider_fault_polling`。kernel build、126 项 IOMMU/DMA CTest 和 512 MiB Global Lock/HID、Intel fault/MSI teardown、AMD
DMA/MSI/no-report teardown QEMU gates 3/3 通过。Global Lock gate 确认设备仍发布 `HID=NAOS0001`、`CID=ACME0001`、`UID=42`；AMD
event 仍为 `first_interrupt=true`，不算 memory-IOPF delivery。ACPI 仍负责固件 inventory 解析/总线 quiescence 与外层 provider
bootstrap 顺序，G11 未完成；G2/G9 的既有门槛仍开放。本轮未暂存或提交。

后续 G11 firmware parser ownership 收敛：DMAR parser 与 device-scope admission 已迁入 `intel_vtd_firmware.cc`，IVRS/IVHD/IVMD 与
device-entry validation 已迁入 `amd_vi_firmware.cc`；`arch/acpi/boot_tables.cc` 只保留通用 ACPI root/table catalog，并通过
`parse_iommu_firmware_inventory` 将表按签名 dispatch 给 owner adapter。kernel build、126 项 DMAR/IVRS/IOMMU/DMA CTest 与 512 MiB
Global Lock/HID、Intel VT-d、AMD-Vi QEMU gates 3/3 通过。当前 ACPI snapshot 类型仍内嵌两种厂商 inventory，DMAR/IVRS requester
resolution helpers 也还在 ACPI boot-table 模块，下一步要把 inventory 数据结构和 resolver 一并纳入 provider adapter 边界。AMD
QEMU event 仍为 `first_interrupt=true`，G2/G9 与 AMD memory-IOPF QEMU 资格仍开放。本轮未暂存或提交。

后续 G11 source-resolution ownership 收敛：DMAR 的 PCI endpoint/sub-hierarchy 和 IOAPIC requester resolver，以及 AMD IVHD 的 PCI
requester/alias 与 IOAPIC resolver，现由各自 `*_firmware.cc` 实现并以 Intel/AMD provider namespace 暴露；vendor-specific resolver
error enum 与声明已从 `arch/acpi/boot_tables.hpp` 移除，VT-d/AMD controller 直接调用所属 adapter resolver。更新了现有
`acpi_boot_tables_test` 的 resolver 命名空间，并让其、`intel_vtd_test`、`amd_vi_controller_test` 链接 provider parser sources。
kernel build 与三个 test executable build 通过，126 项 ACPI/IOMMU/DMA CTest 和 512 MiB Global Lock/HID、Intel、AMD QEMU gates
3/3 通过。ACPI snapshot 仍持有厂商 inventory struct/record vectors，root catalog builder 仍负责分别记录/拒绝重复 DMAR/IVRS；
这些类型与 inventory 生命周期协调仍需迁移。AMD event 仍是 `first_interrupt=true`，G2/G9 与 memory-IOPF QEMU 资格仍开放。本轮未暂存或提交。

后续 G11 typed inventory ownership 收敛：`acpi_boot::snapshot` 现只保存通用 ACPI table catalog、关系、MCFG 与 generation；DMAR
hardware units/scopes/RMRR 和 AMD IVHD/IVMD/device entries 与 parse state 已分别移入
`iommu::intel_vtd::firmware_inventory`、`iommu::amd_vi::firmware_inventory`。`build_snapshot` 不再识别、解析或保存厂商 IOMMU
inventory；provider bootstrap 从 raw ACPI table catalog 查找唯一 DMAR/IVRS，调用所属 parser、应用通用物理范围 validator，并在
adapter 内分配、保存 typed inventory。controller、resolver、register callback 和 aperture lease lifecycle 都只接收 provider-owned
inventory；硬件初始化失败时保留可能仍被 register callback/硬件引用的 inventory。ACPI 现在只提供 generic catalog、range
validators、aperture callbacks 和 requester platform callbacks。host fixtures 显式调用 owner parser；kernel build、126 项
IOMMU/DMA CTest 与 512 MiB Global Lock/HID、Intel VT-d、AMD-Vi QEMU gates 3/3 通过。G11 剩余外层 quiescence/bootstrap 调度和 AMD
memory-IOPF QEMU evidence；G2 父仓库 gitlink 与 G9 PM1 GBL_STS/SCI waiter 联合门槛仍开放。本轮未暂存或提交。

后续 G11 backend discovery ownership 收敛：ACPI 不再单独调用 Intel 与 AMD 初始化函数，也不再根据 DMAR/IVRS 状态分支；它一次
提供通用 bootstrap config。kernel IOMMU owner 扫描通用 ACPI table catalog，只按存在的 DMAR/IVRS signature dispatch 至对应
adapter；重复表检测、解析、typed inventory allocation、firmware policy gate、aperture admission、controller 初始化和 provider
注册均在所属 adapter/backend 路径。Intel/AMD adapter 接管原启动成功/失败诊断 marker，保持现有 QEMU 门禁合同。kernel build、
126 项 IOMMU/DMA CTest 与 512 MiB Global Lock/HID、Intel、AMD QEMU gates 3/3 通过。ACPI 仍负责 PCI DMA quiescence 与组装通用
platform callbacks；G11 尚缺 AMD memory-IOPF QEMU 资格，G2 父仓库 gitlink、G9 PM1 GBL_STS 到 SCI waiter wake 联合门槛仍开放。
本轮未暂存或提交。

2026-10-26 G11 conformance gate 增补：新增单一 `iommu_backend_conformance.hpp`，由 Intel VT-d 与 AMD-Vi controller tests
各自调用同一组断言，并通过各自真实 controller/fake register-memory-cache callbacks 执行。共同覆盖 requester default-deny、
DMA read/write permission encoding、地址宽度与非法权限拒绝、domain/page create-map-unmap-destroy 对应的 context/DTE 与 IOTLB
invalidation、fault-log overflow 上报、interrupt-remapping route 安装/撤销及其 invalidation，以及未知 invalidation completion
后 domain quarantine、后续操作拒绝和硬件可见 page-table 保留。两 backend conformance 用例通过；完整
`intel_vtd_test` 55 cases/1073 assertions、`amd_vi_controller_test` 16 cases/376 assertions 通过。Intel overflow 由 provider adapter
上报给公共 fault-source owner 处理，AMD controller 同时 quarantine unit；未知 completion 的 fail-closed 由两家共同用例验证。
这关闭了 G11 明确要求的共享 fake-callback conformance suite 缺口，但不关闭 G11：AMD memory-IOPF QEMU delivery/owner teardown
证据仍不足。G2 父仓库 gitlink 与 G9 PM1 GBL_STS/SCI waiter 联合验收继续开放。本轮未暂存或提交。

同日 ACPI/provider 边界清理：`acpi.hpp` 中遗留的 “AMD-Vi polling”“DMAR scope”“VT-d prerequisite” 描述改成 provider-neutral
契约；DMA source admission 依赖 `device_source_ref::valid()`，不再重复列出 vendor。AMD fault polling 成功 marker 与 aperture
reserve/map 成功 marker 均由 AMD adapter 在对应资源/定时器成功后输出，ACPI 不再特判 AMD polling。kernel build 与两套完整
controller tests 通过；512 MiB `q35_amd_vi_boot` 和 EDU DMA/MSI/no-report teardown trace gates 各 1/1 通过。首轮 boot gate
因 adapter 抽离时遗漏 aperture marker 失败，补回 adapter 成功路径后复验通过。AMD 48882 §2.5.3/Table 57 规定 I=1 表示
interrupt request、I=0 表示 memory request；当前 QEMU `amdvi_page_fault` 无条件置 I，因此 EDU 的 event 不能作为 memory-IOPF
delivery 证据，kernel 保留按规范过滤。参见 [AMD IOMMU Specification](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/specifications/48882_IOMMU.pdf)
与 [QEMU AMD-Vi event encoder](https://github.com/qemu/qemu/blob/master/hw/i386/amd_iommu.c)。G11、G2、G9 均仍开放。本轮未暂存或提交。

2026-10-26 G2 fork test sweep：在 submodule 的 ACPICA build 输出上设置 `PATH=build-debug/acpica-iasl/bin:$PATH` 后，执行完整
`cargo test --workspace --offline`。首轮发现 fork 的兼容 API `set_interrupt_model_used()` 仍绕过新增的 one-shot `_PIC` 状态机，
也没有开放 `_PRT` gate；原 package-name reference 测试因此在读取 `_PRT` 时得到
`NOT_SUPPORTED_PRT_BEFORE_PIC`。该 API 现使用 `evaluate_pic_once()`，成功（包括 firmware 无 `_PIC`）后才开启 routing methods；
测试覆盖有/无 `_PIC` 两种情形、拒绝第二次调用，并能随后解析 absolute/relative `_PRT` source。修复后的 workspace 全部通过
（178 passed，5 ignored），`cargo fmt --all -- --check` 和父仓库 `cargo test --locked --workspace` 通过。自定义 NaOS toolchain 的
`platformd` build 通过；512 MiB `q35_acpi_global_lock_hid_boot` 复验 1/1，serial 确认 FACS Global Lock SCI ready 且发布
`HID=NAOS0001`、`CID=ACME0001`、`UID=42`。G2 源码与测试现在通过 fork submodule 工作树运行，但仍未提交到 fork revision、父仓库
gitlink 也未更新；用户要求先完成功能、不提交，所以 G2 pin 门槛保持开放。本轮未暂存或提交。

2026-10-26 G11 AMD memory-IOPF QEMU 正向门槛通过：检查官方 QEMU v11.1.1 源码后，测试夹具
[`qemu-11.1.1-amdvi-memory-iopf.patch`](../tests/fixtures/qemu-11.1.1-amdvi-memory-iopf.patch) 让 emulator 按 AMD 48882 Rev 3.11
编码 memory IOPF 的 `I=0`、`PR/PE/RW`、DomainID，并修正 `amdvi_setevent_bits()` 将第二 qword 地址清零的位掩码 bug；guest 仍严格
过滤 interrupt IOPF。在干净 QEMU v11.1.1 checkout 应用夹具并构建，再运行 NaOS gate：

```sh
git -C /path/to/qemu-v11.1.1 apply --check /path/to/naos/tests/fixtures/qemu-11.1.1-amdvi-memory-iopf.patch
git -C /path/to/qemu-v11.1.1 apply /path/to/naos/tests/fixtures/qemu-11.1.1-amdvi-memory-iopf.patch
ninja -C /path/to/qemu-v11.1.1/build qemu-system-x86_64 -j4
cmake -S . -B build-debug -DNAOS_PCIED_MSI_ROUTE_SMOKE=ON -DNAOS_TEST_AMDVI_MEMORY_IOPF=ON
cmake --build build-debug --target make-root-image -j2
PATH=/path/to/patched-qemu-bin:$PATH ctest --test-dir build-debug \
    -R '^q35_pci_amd_vi_dma_msi_fault_trace_boot$' --output-on-failure -j1
```

512 MiB QEMU CTest 1/1
通过；serial 验证原始事件 `2070000000000018:0000000000005000`、requester/IOVA/write-direction fault delivery、source close 后第二批
fault drain、domain/MSI teardown 和 child exit status 0；QEMU trace 断言 PCI command register 写入 `0x103`（BME clear）。该结果关闭
G11 AMD fault delivery、BME disable 与 test-owner teardown 的 QEMU gate；普通未修补 QEMU 仍不能用于此正向事件测试。本轮未暂存或提交。

2026-10-26 G9 PM1 Global Lock waiter QEMU 联合门槛通过：新增
[`qemu-11.1.1-acpi-global-lock-sci.patch`](../tests/fixtures/qemu-11.1.1-acpi-global-lock-sci.patch)，给 QEMU ICH9 PM block 增加仅用于
opt-in integration test 的 firmware-event stimulus。测试内核在 waiter 阻塞后向该端口写入；QEMU 将 PM1 `GBL_STS` 置位并调用正常
SCI 更新路径，SCI 经 QEMU IOAPIC 到达 NaOS。生产 `fixed_hardware` 不再合并测试状态位或直接调用 `on_interrupt()`；handler 只根据实际
PM1 status register 唤醒 waiter。第二次 stimulus 在 `platformd` claim SCI 后发送，验证 typed drain/clear/ack。配置
`NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE=ON`、应用该 QEMU fixture 并运行 512 MiB `q35_acpi_global_lock_waiter_wake_boot`，CTest 1/1 通过；
同一构建下 `q35_acpi_global_lock_hid_boot` 也通过 1/1，
QEMU trace 记录两次 `ich9_acpi_global_lock_test_gbl_sts`，serial 两次从 PM1 读到 `status_a=0x21`（含 `GBL_STS=0x20`）；被唤醒的
有限等待者按预期返回 timeout（result 5），`platformd` 确认第二次 Global Lock status 并完成 clear/ack，`_GLK` 设备保持 healthy 并继续发布
HID/CID/UID。该门槛验证真实阻塞 waiter 的 SCI 唤醒与 deadline 结果，不代表竞争者释放后 waiter 成功取得 FACS lock。该 fixture 明确模拟
runtime-firmware status event；FACS 竞争者由仅在 opt-in build 编译的 test actor 模拟，但 PM1 status、GBL_EN、SCI/IOAPIC、内核 handler
与 platformd ack 都经过 QEMU 的设备模型路径。它不声称验证物理 SMM firmware。复现命令：

```sh
git -C /path/to/qemu-v11.1.1 apply --check /path/to/naos/tests/fixtures/qemu-11.1.1-acpi-global-lock-sci.patch
git -C /path/to/qemu-v11.1.1 apply /path/to/naos/tests/fixtures/qemu-11.1.1-acpi-global-lock-sci.patch
ninja -C /path/to/qemu-v11.1.1/build qemu-system-x86_64 -j4
cmake -S . -B build-debug -DNAOS_PCIED_MSI_ROUTE_SMOKE=ON -DNAOS_TEST_ACPI_GLOBAL_LOCK_WAKE=ON
cmake --build build-debug --target make-root-image -j2
PATH=/path/to/patched-qemu-bin:$PATH ctest --test-dir build-debug \
    -R '^q35_acpi_global_lock_waiter_wake_boot$' --output-on-failure -j1
```

G9 的 QEMU 验收门槛现已完成，G2 fork gitlink 仍待固定。本轮未暂存或提交。

2026-10-27 G2 dispatch 与 malformed-AML 复验：`NativeRegionHandler` 现明确执行 authorize -> opaque token -> token-relative access；
dynamic OpRegion 重定义先撤销 token，`_REG(0)` 回调运行前先断开 handler registration。新增/扩充的 fork 回归覆盖 raw Handler 无 fallback、
dynamic region replacement、disconnect callback access denial、撤销 token 拒绝、QWordField overflow、BufferField write bounds、短 NameSeg、
VarPackage allocation budget、Store reference cycle 和 If/Else PkgLength underflow。fork `cargo test --workspace --offline`、NaOS
`cargo test --locked --workspace --target x86_64-unknown-linux-gnu --offline` 与 format/diff checks 通过。stage1 `platformd` build 后，512 MiB
`q35_acpi_global_lock_hid_boot`、`q35_acpi_global_lock_waiter_wake_boot`、`q35_pci_amd_vi_dma_msi_fault_trace_boot` 均通过 1/1；AMD
trace 验证 requester `0018`、write fault `0x5000..0x503f`、BME clear 与 teardown。QEMU waiter gate 观测到 SCI 唤醒后有限等待按预期 timeout，
随后完成 PM1 `GBL_STS` clear/ack，不声称 waiter 在该 fixture 中成功抢到 FACS lock。

同日 60 秒 bounded fuzz：`aml_load` 1,077,392 executions、最高 RSS 41 MiB；`aml_execute` 603,059 executions、最高 RSS 30 MiB；
`resource_template` 10,856,946 executions、最高 RSS 29 MiB，均无 crash、timeout 或 OOM。build-local AML corpus 保留了前几轮发现的
VarPackage OOM、NameSeg、reference-cycle、Match index 和 If/Else length reproducer；确定性回归覆盖已修复路径。Fuzz corpora 与 crash
artifacts 均在 `build-acpi-pci/`，未纳入源码。

G2 的行为与 QEMU 验收项现已通过；正式门槛只剩 fork commit 和父仓库 gitlink 固定。用户要求暂不提交，本轮仍未暂存或提交。

2026-10-27 AML Global Lock 与 PCI/QEMU 收尾复验：fork 的 AML `Acquire` 现按规范把成功/超时 Boolean 写回 AML 表达式；method
退出时仅释放该 method 新取得的 Global Lock 深度，覆盖正常返回与执行错误。kernel acquire outcome 不确定时，executor 进入 sticky
fail-closed 状态；`platformd` 的 Global Lock 等待期限跟随 kernel request deadline 并留出 reply grace，避免用户态 waiter 先行退出后
遗留仍会在后台完成的 acquire。kernel 另外拒绝 FACS acquire 尚 Pending 时的同线程重入，并用同一 PM1 control 更新锁串行化
`GBL_RLS` 与 sleep commit 的 read-modify-write。非法 `_STA` 不再继承默认 present 状态；PCI topology 可按 `_STA` 的 present 与
functioning 位过滤，`pcied` 分页接受合法 source cursor gap，同时要求游标前进且不越界。

验证：fork workspace 全量测试在其测试所需的 ACPICA compiler 加入 `PATH` 后通过：
`PATH="$PWD/build-debug/acpica-iasl/bin:$PATH" cargo test --manifest-path naos/third_party/rust-acpi-naos/Cargo.toml --workspace --offline`
（工作目录为父仓库）；父仓库 `cargo test --locked --workspace --offline` 也通过。kernel、`pcied` 和
`make-root-image` 构建通过；Global Lock PM1 control 的 C++ 定向回归 2/2 通过，Rust/C++ format check 与 `git diff --check` 通过。
使用 512 MiB guest、带 AMD memory-IOPF 与 Global Lock SCI 测试夹具的 QEMU 11.1.1，完整 `ctest --test-dir build-debug -R '^q35_'`
12/12 通过（505.09 秒）：PCI discovery、VT-d remapping、AMD-Vi、SCI/GPE、HID/Global Lock waiter、EDU DMA/MSI、AMD memory IOPF
delivery/owner teardown、多驱动 grant、MSI-X、Virtio Block I/O 和无 IOMMU DMA quiescence。另修正测试门禁中过时的固件/IOAPIC log
marker，并让 Intel EDU gate 的 fault assertions 随本次构建 feature 匹配实际 read-only IOVA 与 close-drain 场景；测试专用启动期限
从 20 秒调整为 45 秒。失败断言修正后单项和整组 Q35 门禁均通过。

实现与 QEMU 验收行为已完成；G2 的可复现交付门槛仍因 fork revision 未提交且父仓库 gitlink 未固定而开放。按用户要求，本轮未
暂存或提交任何改动。

2026-10-28 PRD 一致性复核：修正 PCI/MSI 主体中仍把 AMD memory-IOPF/owner teardown 写成 G11 未验收的过期结论；现行判断与
门槛表及 2026-10-26 的规范修正 QEMU 记录一致，G11 核心门槛通过，生产异构 driver 和更广的恶意 DMA 验证仍属于未完成的生产化范围。
重新验证父 workspace `cargo test --locked --workspace --offline` 与 fork 全量 workspace 测试均通过；fork 测试需把构建树中的
`acpica-iasl/bin` 加入 `PATH`。G2 仍是唯一未关闭的 PRD 合入门槛：fork 移植改动需要一个可复现 commit 和父仓库 gitlink pin；依用户
要求，没有创建 commit、推送或改动暂存区。此刻父仓库 `HEAD` 尚无该 submodule；index 中已有的 staged entry 指向
`a17f91e8b67823bf4396e45622984bce1b6d38f5`（`acpi` 6.1.1），staged `.gitmodules` 仍指向 `rust-osdev/acpi`。工作树的
`.gitmodules` 才指向 `kadds/acpi`，submodule 工作树基于远端 `main` 的 `ad7fb092596eb0fd1c89400d256ec5c9363c0406`
（`v7.0.0-pre1`），并含尚未提交的 NaOS 移植。`cargo tree --locked --offline -p rust-acpi-naos -i acpi` 确认当前构建使用此
path dependency；这验证的是当前工作树，不能代替从父仓库提交干净检出后的构建验证。该差异是既有暂存状态；本次审计保留它不变。
未来关闭 G2 时，需将 fork 改动固化到 fork commit，使父仓库提交的 URL 与 gitlink 一起指向该 revision，再从干净检出运行锁定构建与测试。

同日 G11 requester callback 命名复核：`arch::ACPI` 提供给 `pci_requester_platform_ops` 的 ECAM presence/BME 查询现使用
`pci_requester_present` 与 `pci_requester_bus_master_enabled` 名称，不再把通用平台回调标成 AMD 专属；IOMMU provider 选择与 fault
delivery wiring 仍通过 provider-neutral dispatch/report。`cmake --build build-debug --target kernel -j2` 通过；当前源码在宏关闭的
`build-acpi-pci` 上以未修改的系统 QEMU 11.1.1 重建后，`q35_amd_vi_boot` CTest 1/1 通过，serial 确认 AMD-Vi aperture、default-deny、
fault polling、PlatformPciTopology 和 `pcied: INFO: ready regions=1 roots=1 devices=7`，未见 panic/oops。
该 boot gate 使用系统 QEMU，未修改 emulator。另一次在 `build-debug`（`NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE=ON`）使用未打 Global Lock
SCI fixture 的系统 QEMU 时，waiter 测试配置与 emulator 不匹配，`q35_amd_vi_boot` 未到达 topology markers；该结果不计作 AMD callback
正向验收。AMD memory-IOPF 正向 gate 仍只以 PRD 前文所述的规范修正 QEMU fixture 结果为证，未在本次重新构建或修改 QEMU。

历史 Global Lock 软件串行化复核曾把 GBL event 不可用也当作本地软件锁条件；该分支已由 2026-10-28 的 FACS 共享仲裁规则撤回。
当前只有无 FACS 或 `HW_REDUCED` 才使用 PID/TID owner/depth 与 wait queue 软件串行化；FACS 存在但 GBL event 不可用时仍 CAS 并轮询，
已启用后丢失则 fail-closed。相关 `select_global_lock_arbitration_path` 回归覆盖无 FACS、`HW_REDUCED`、event absent、正常 FACS 和
event-lost 分支；Global Lock/PM1 C++ 回归与 Q35 HID gate 证据见后续记录。
`q35_acpi_global_lock_hid_boot` 1/1 通过，确认常规 FACS 路径仍发布 `HID=NAOS0001`、`CID=ACME0001`、`UID=42`。该 QEMU gate 验证硬件路径；
同日新增的 fallback QEMU gate 之前尚未完成。

同日 Global Lock fallback 竞态修正：获取本地 owner slot 后，arbitration path 现在在同一 kernel lock 临界区内检查 sticky
`global_lock_event_lost`；若 event path 已丢失，先撤销本次 local reservation、唤醒等待者并返回 unsupported，不再把已启用后丢失的
硬件协议误判为“硬件不存在”而切换软件 fallback。只有 FACS 缺失或 `HW_REDUCED` 时才保持 PID/TID owner/depth 与 wait queue 的
本地串行化并省略 FACS CAS；FACS 存在时，即使 GBL event 明确不可用且未进入 lost 状态，也继续 CAS 并轮询 FACS word。
FACS transition、path-selection、PM1 update C++ 回归
3/3 通过，kernel build 通过；宏关闭的 512 MiB stock-QEMU `_GLK` HID gate 1/1 通过，serial 同时确认 FACS hardware path 正常并发布
HID/CID/UID。

同日 Global Lock software fallback QEMU gate 通过：新增 `NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK` 仅在 opt-in kernel build 中
强制 acquire 选择本地 software serialization 分支；Q35 固件仍提供 FACS/SCI，没有修改 QEMU、固件表或 `util/run.py`。512 MiB stock-QEMU
`q35_acpi_global_lock_software_fallback_boot` CTest 1/1 通过；serial 依序确认 test override、local software fallback 选择、`_GLK`
设备 healthy 以及 `HID=NAOS0001`、`CID=ACME0001`、`UID=42`。fixture 的 `_INI` 在 `_GLK` 保护范围内执行嵌套显式
`Acquire/Release`；Rust AML interpreter 在同一执行线程上处理该重入，因此 kernel 收到并持有外层 owner slot，直到 `_INI` 结束。
该 gate 验证硬件被 kernel 明确判为 absent 时的真实 AML fallback；它不模拟物理上缺失的 FACS。新增 owner-claim C++ 回归覆盖
kernel 状态分类中的 claim、递归、pending、不同 PID/TID 竞争、深度溢出和损坏状态。QEMU gate 没有创建第二个独立 AML executor
来验证实际 wait/wake 竞争；kernel contender 路径仍由 owner slot、wait queue 和 wake sequence 实现。

同日 PCI root `_STA` 过滤缺口修复：此前最终 root scope 和 topology 在 `snapshot_devices()` 求值 `_STA` 之前建立，且 root 发布未消费
设备状态。现在先执行一次设备快照，再以 root/device 的 namespace path、device ID、generation 和已求值的 present/functioning 位筛选 root；
只有筛选后的 root 才绑定最终 scope、安装正式 PCI_Config selectors、协商 `_OSC`、创建 resource attestation 并发布给 `pcied`。启动期
provisional selector 仍仅供 AML 初始化求值使用。`platformd` 单测覆盖 active、absent、non-functioning、未求值和 stale-generation 状态；
512 MiB stock-QEMU `q35_acpi_disabled_pci_root_boot` 1/1 通过，注入的 `\_SB_.OFF0` 有 `PNP0A08` 与合法 `_CRS` bus window、但 `_STA=0`，
串口确认它未进入 topology，`pcied` 最终仍只收到一个 root。`cargo test --locked --workspace --offline`、NaOS kernel/服务
`make-root-image` 构建和格式检查通过。

同日 G3 账本与 SCI worker 生命周期复核：namespace value/level 现在持有对应的 RAII 预算 lease；覆盖插入被 immutable 值拒绝、可变值替换、
child level 删除和动态 OpRegion parent path。lease drop 同时归还 namespace object count 与 persistent heap；OpRegion 路径 lease 跟随
`WrappedObject` 生命周期。`ExecutionMeter` 增加 namespace-object release 回调。worker 在派发 queued async request 前检查 oneshot receiver 是否已关闭，
避免已超时的排队 GPE 请求仍执行；SCI 10 秒 timeout 会再向 worker 排队幂等 `EndSciWorkUnit`，释放 worker-owned aggregate guard。已开始运行的同步
AML handler 不会被强行中断，cleanup 在该 handler 返回后执行。namespace lease 的 `ObjectHeapReservation` trait 增加 `Send` bound，保持
`Interpreter: Send + Sync`；fork workspace 原有静态断言曾捕获该属性回归，修复后通过。

验证：父 workspace `cargo test --locked --workspace --offline` 与 fork 全 workspace
`PATH="$PWD/build-debug/acpica-iasl/bin:$PATH" cargo test --manifest-path naos/third_party/rust-acpi-naos/Cargo.toml --workspace --offline`
均通过；fork 的 AML library 111 项通过，platformd 定向测试 61/61 通过。父 workspace 与 fork 触及文件的 Rust
format check、父仓库及 fork `git diff --check` 通过。`make-root-image` 在宏关闭的 `build-debug` 与启用软件 fallback 的 `build-acpi-pci` 均通过。
这两个 QEMU build 都在 `ObjectHeapReservation: Send` 修复后重新构建。512 MiB stock-QEMU
`q35_acpi_global_lock_software_fallback_boot` 1/1 通过，serial 确认 local software fallback 与 `_GLK` 的 HID/CID/UID；普通
FACS hardware path 则在 `NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE=OFF`、software-fallback override 关闭的 `build-debug` 上通过
`q35_acpi_global_lock_hid_boot` 1/1，serial 确认 FACS SCI ready 和同一组 HID/CID/UID。测试未修改 QEMU。G2 fork commit/gitlink pin 仍是唯一
未关闭的 PRD 交付门槛；本轮没有暂存或提交。

同日 G2 native token cache 事务复核：`NativeRegionAdapter::token_for` 此前可能在 backend 已授权 lease 后才提交 path heap，预算提交失败时没有
释放该 lease；generation 查询与授权发生竞态时，也会先把 token 按旧 generation 缓存再返回 stale 错误。现在在 kernel authorization 前完成两类
cache heap 预留与 working-to-persistent 转移；返回 generation 不匹配时立即释放授权 lease。若 release 失败，将 token 以不可用于 AML 的状态留在
受预算约束的 cache 中，供后续重试或 lifecycle invalidation 回收。新增测试分别验证成功 rollback 后 heap 归零且后续请求重新授权，以及 rollback
失败时 lease 仍可由 teardown 重试释放。父 workspace `cargo test --locked --workspace --offline`、build-debug 与 software-fallback build 的
`make-root-image` 均通过；同两种 build configuration 下 512 MiB stock-QEMU hardware Global Lock/HID 与 software-fallback gates 各 1/1 通过。
G2 的 source 行为和本地验证通过；fork commit/gitlink pin 仍受“先别提交”约束而未关闭，本轮未暂存或提交。

2026-10-28 Global Lock arbitration 边界修正：对照 ACPI 6.5 §5.2.10.1 后，撤销此前“FACS 存在但 GBL SCI 不可用时改走本地软件锁”
的实现。FACS lock 是 OSPM 与平台固件共享的锁，因此 FACS 存在且非 `HW_REDUCED` 时始终通过 FACS CAS 仲裁；GBL SCI enable
不可用时以 1 ms wait-queue deadline 轮询 FACS lock word，继续观察固件释放。只有 FACS 缺失或 `HW_REDUCED` 才使用 kernel PID/TID、
递归深度和 wait queue 做本地软件串行化。已启用后丢失的 SCI 路径仍 fail-closed。`NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK`
现在只在 acquire 的测试决策点强制 no-FACS 软件路径；q35 固件仍正常提供 FACS/SCI，测试没有伪称模拟物理 FACS 缺失，也未修改 QEMU。
path-selection、owner-claim 和 PM1 update 定向 C++ 回归在宏关闭 build 通过 3/3；`build-debug` 与 `build-acpi-pci` kernel build 通过。
512 MiB stock-QEMU 软件 fallback `_GLK` boot gate 与标准 FACS/SCI HID/CID/UID boot gate 各 1/1 通过。使用 build-local、应用仓库
`qemu-11.1.1-amdvi-memory-iopf.patch` 的 QEMU 11.1.1，AMD memory-IOPF positive gate 1/1 通过；系统 QEMU 对该 gate 的既有字段
编码差异不作为正向验收。旧日志中“GBL event absent 即本地 software fallback”的描述由本条修正。

同日最终 Q35 全集复验：在刷新 `build-debug` root image 和 disabled-root AML fixture 后，使用上述 build-local QEMU 11.1.1 执行
`ctest --test-dir build-debug -R '^q35_' --output-on-failure -j1`，12/12 通过，用时 530.51 秒。覆盖 PCI discovery、Intel VT-d、AMD-Vi、
SCI/GPE、Global Lock/HID、disabled PCI root、MSI/MSI-X、AMD memory-IOPF、multi-driver grant、virtio-block MSI I/O 与 DMA quiesce；
测试序列未见失败或内核 panic/oops。此前 stock-QEMU 下 9/12 的集合结果已由本次复验覆盖：discovery 重跑通过、fixture 依赖已修复，
AMD IOPF 使用规范 fixture 后通过。全程没有提交或暂存项目改动；G2 fork commit/gitlink pin 仍待 fork revision 固化后才能关闭。

同日 G9 waiter 联合门槛在本次 Global Lock 改动后复验：`build-acpi-rollback` 临时关闭 release-failure 注入、保留
`NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE=ON`，用包含仓库 GBL_STS fixture 的 build-local QEMU 11.1.1 运行
`q35_acpi_global_lock_waiter_wake_boot`，CTest 1/1 通过（32.45 秒）。serial 显示 firmware actor 持有 FACS lock，QEMU 置位
`GBL_STS` 并经 SCI 唤醒真实阻塞 waiter；PM1 Global Lock status 被观察两次，有限等待结果为 timeout，`platformd` 完成 clear/ack
并记录 wake sequence 1。没有 panic/oops。测试后已恢复 build cache 的 release-failure 配置；父仓库与 fork 均未暂存或提交。

2026-10-29 Global Lock 软件串行化与配置切换复验：无 FACS 或 `HW_REDUCED` 时，软件 owner lock 与 wait queue 不依赖 SCI；
`event_lost` 不能误禁用这条路径。FACS 存在时仍保持共享 FACS CAS，GBL event absent 轮询，已启用 event 丢失则 fail-closed。
SCI 在启动时从未建立 GBL event 路径时也不再误记成“运行中丢失”，保留 FACS CAS+轮询；只有此前确认可用的 event 后续失效才隔离
acquire。新增 C++ path-selection 与 event-loss 转换回归均通过。512 MiB stock-QEMU
`q35_acpi_global_lock_software_fallback_boot` 1/1 通过，实际 AML `_GLK` `_INI` 嵌套 `Acquire/Release` 后仍发布 healthy
设备及 `HID=NAOS0001`、`CID=ACME0001`、`UID=42`；该 opt-in gate 强制走本地软件分支，没有模拟物理 FACS 缺失，也未测第二个
独立 AML executor 的竞争唤醒。build-local `build-acpi-rollback` 上 owner-exit 和 release-failure gate 各 1/1 通过；切换 Cargo
feature 时，新增的 CMake Rust build-config input 会触发 Rust 构建及安装产物更新，`strings` 核对 installed `platformd` 与当前
feature 一致。宏关闭 kernel、软件 fallback、owner-exit、release-failure 配置均已分别构建；owner-exit 测试后已恢复原 release-failure
build cache。未暂存或提交。

2026-10-29 当前工作树综合复验：父 workspace `cargo test --locked --workspace` 通过；fork
`cargo test --manifest-path naos/third_party/rust-acpi-naos/Cargo.toml --workspace --offline` 通过，AML library 111 项通过。
宏关闭 `build-debug` 的 kernel build 及 `make-root-image` 通过。使用 build-local QEMU 11.1.1 重跑完整 512 MiB Q35 组，CTest
12/12 通过（531.42 秒）；覆盖 PCI discovery、VT-d、AMD-Vi、SCI/GPE、Global Lock/HID、disabled root、MSI/MSI-X、AMD memory-IOPF、
multi-driver、Virtio Block I/O 与 no-IOMMU DMA quiesce。最终 Global Lock 改动后的 `q35_acpi_global_lock_waiter_wake_boot`
单独复验也通过（1/1，32.84 秒），trace 含两次真实 PM1 `GBL_STS` 置位，serial 确认 waiter wake、timeout 与 owner ack。
ACPI boot-table、physical-resource、`_OSC` authority、PCI discovery-scope、fixed-hardware C++ suites 共 103 个 case/1,856 assertions
通过。父 workspace 与 fork `cargo fmt --check`、全部改动 C/C++ 的 `clang-format --dry-run --Werror`、父仓库与 fork diff checks
通过；宏关闭 kernel 在格式化后重新构建通过。G1–G15 中当前唯一未关闭的正式门槛仍为 G2：fork 移植尚未固化为远端 commit，
父仓库 gitlink 仍未指向包含移植的 revision。按用户此前要求，本轮没有提交或新增暂存。
