# ADR：MemoryObject 共享页语义

- 状态：Accepted（W1 已实现，`boot_smoke_mobj_share` 通过）
- 日期：2026-09-11
- 决策者：NaOS kernel 维护者
- 适用范围：`NA_SYSCALL_MEMORY_MAP`、`MemoryObject` 的页缓存、`fork`/COW 与
  `File.materialize` 的私有映射桥
- 相关 ADR：[Capability Handle 与异步 Invocation IPC](OBJECT_CALL_ADR.md)、
  [用户态文件系统](USERSPACE_FILESYSTEM_ADR.md)

本 ADR 只描述 MemoryObject 映射是否共享物理页这一项新机制。它不改变
`OBJECT_CALL_ADR.md` 关于 Channel、Invocation/Responder 与 readiness 的决策，
也不引入新的内核对象类型。

本 ADR 与 MemoryObject bounded view 一起使用：`MemoryObject` 是 storage identity，
`MemoryView` 是带有有限 `(offset, length)` 的 capability，`VmMapping` 是地址空间
投影，COW clone 则产生新的 storage identity/version。MemoryView 不拥有页，也不把
view 变成另一个内核对象；跨 IPC 只转移这个受限 capability。

## 1. 背景

`NA_MEMORY_MAP_SHARED` 此前只影响"拆除映射时是否回写"：缺页时内核总是新分配一页
私有页并从对象 `read()` 拷入，因此

- 两个 SHARED 映射同一对象同一偏移的进程无法互相观察写入；
- 共享写入不落在对象上，只有 unmap 时才整段拷回；
- 每次请求都在缺页路径上引入一次载荷拷贝和一次页表更新。

上层已经从 POSIX 侧使用共享语义：mlibc 的 `mmap(..., MAP_SHARED, fd, ...)`
直接设置 `NA_MEMORY_MAP_SHARED`，`smoke` 的 framebuffer 生命期用例依赖它；
`libnao.hpp` 的 `MemoryObject::map()` 默认标记 SHARED。也就是说，"SHARED" 在
用户可见语义里已经承诺共享，缺失的只是内核实现。

## 2. 决策

### 2.1 对象页缓存即映射目标

`memory_object` 拥有一个按页组织的缓存（`pages_`），由
`publish_shared_pages()` 一次性建立：

1. 仅当对象拥有可变字节存储（既不是不可变外部视图，也不是设备直映射）时建立；
2. 为一个 `(memory_object, page_index)` 分配恰好一页，整页清零（对象尾部不得
   泄露内核内存），再拷入对象既有字节；
3. 建立后**释放原字节向量**，页缓存成为对象唯一存储。

因此内核视图（`read`/`write` IPC）与所有 SHARED 映射读写的是同一组物理页。
对象尺寸与 `NA_MEMORY_OBJECT_MAX_BYTES` 上限不变：页缓存只是在建立时把逻辑尺寸
向上取整到页边界，`read`/`write`/`size()` 仍以逻辑尺寸判定边界，尾部填充字节
不对外可见。

`publish_shared_pages()` 对不可变外部视图（boot archive 的零拷贝视图）与
direct-mapped 设备内存（framebuffer）返回 `NA_STATUS_NOT_SUPPORTED`：它们已经
发布了自己的存储，不需要也不允许再有第二份页缓存。空对象同样不建立。

### 2.2 SHARED 与 PRIVATE

| | `NA_MEMORY_MAP_SHARED` | 私有映射（未置位） |
| --- | --- | --- |
| 缺页 | `map_to()` 对象页帧，0 次载荷拷贝 | 分配私有页并 `read()` 拷入 |
| 写可见性 | 对所有 SHARED 映射与对象立即可见 | 仅本进程可见 |
| 拆除 | 不需要回写（`release_frames=false`，帧归对象） | 需要回写时保留既有 COW/回写路径 |
| fork | 保持共享，见 §2.3 | COW |

映射只有在其对象范围覆盖**整个 VMA 且逻辑范围页对齐**时才标记为共享
（`map_t::pages_shared`）。
`map_memory_object` 只接受 `object_offset + map_length <= object->size()` 的
SHARED 映射；否则返回 `nullptr`。这样 `pages_shared` 是一个覆盖整个 VMA 的
不变量，缺页时取不到帧就只可能是内核不变量被破坏，直接 fault，而不是悄悄退回
私有页——后者会让 SHARED 映射失去别名语义。

### 2.6 Bounded view 与非页对齐范围

`MemoryObject::subspan(offset, length)` 通过 duplicate 后的 capability 做
`NA_RESTRICTION_RANGE`，范围相对于父 capability，并验证
`offset + length <= parent.view_length`。结果 capability 仍指向同一 storage identity，
只更新 capability table 的 range metadata；因此任意持有子 view 的 client 都不能用
另一个 offset 访问父 view 之外的字节。Linux 的等价实现复用同一个 memfd/`Arc<File>`，
ResourceDescriptor 携带相同的绝对 range，并只允许 identity-preserving 或进一步
attenuated 的 descriptor。

Memory mapping 的 syscall offset 始终相对于传入 view。内核内部把实际 storage
offset 向下对齐到页，并在 `MemoryMapFrame.data_offset` 返回 leading offset，同时
保存逻辑 `data_length`。非页对齐
范围的边缘页使用私有、清零后只加载逻辑交集的页；销毁映射时只回写逻辑交集。这样
普通 byte pointer 不会因为页粒度而看到 view 外的 storage bytes。只有逻辑范围完整
覆盖页边界时才使用对象页缓存的直接 alias；因此 unaligned view 仍可用，但不会错误
地把相邻字节暴露给 client。

需要跨 IPC 长期保留并直接读写的共享窗口必须把 MemoryObject 的分配尺寸和持久映射
尺寸向上取整到页边界；本次传输的逻辑 size 仍可以是任意字节数。servicekit
提供 page_aligned_size，NaOS map_persistent 也会拒绝非页对齐长度。短期的非页
对齐请求映射仍走私有页与 unmap 回写语义，不能当作长期共享窗口使用。

IDL 数据面只传 bounded buffer capability；请求里的 `size` 仅表示本次操作长度。
这消除了“capability 已经受限但 wire offset 又可自由构造”的重复边界，并使 NaOS
与 Linux 的 admission/access 检查保持一致。该接口尚未上线，schema revision 已直接
前进，不保留旧 wire layout 的兼容承诺。

### 2.3 与 fork/COW 的交互

`info_t::share_to` 会让 `clone_readonly_to()` 把两侧用户映射统一改成只读 +
COW。对私有内存这是正确行为；对 SHARED 映射则是错的：帧属于对象，不属于任一
进程，任何一侧都不许把它私有化（`info_t::copy_at` 因此拒绝为 `pages_shared`
映射做 COW，缺失页表项时按访问违规处理）。

因此 `share_to()` 在克隆两侧各自调用 `restore_shared_memory_mappings()`，把
每个可写的、整段覆盖的 `pages_shared` VMA 重新 `map_to()` 到对象页帧并清除该
VMA 的 COW 标记。只读映射不参与（它们不会触发写 fault）。若某个 VMA 未能整段
还原，则保留其 COW 标记，避免留下"写进去再也出不来"的 VMA。

### 2.4 权限按映射判定

对象的创建权限（`NA_MEMORY_RIGHT_READ/WRITE/MAP/INFO`）不变，仍然是
全部映射的准入依据，在 `syscall::memory_map` 内集中判定：`MAP_WRITE` 需要
`NA_MEMORY_RIGHT_WRITE`，`NA_MEMORY_FLAG_READ_ONLY` 的对象直接拒绝
`MAP_WRITE`。

页缓存不放松这层判定：SHARED 与否只决定页帧来源，不决定权限。映射的读写
权限仍由页表表征。**对象页缓存一旦成为权威就不能再从用户映射"写回"**，
因此 `write_back_memory_object` 对 `pages_shared` 直接返回成功。

### 2.5 生命周期与引用计数

- 页帧由 `memory_object` 拥有，在 `~memory_object()`（即最后一个 capability
  关闭）时整组释放。
- 用户映射只借用帧：`page_table_t::unmap(..., release_frames=false)` 在拆卸
  SHARED 映射时不清除页帧，避免内核 double free。内核堆分配页的普通映射仍走
  `release_frames=true`。
- 私有映射的页帧仍归地址空间所有，路径不变。
- 对象地址范围在映射建立时校验，映射建立后对象尺寸不再变化（目前没有
  resize 接口），因此不需要按映射跟踪对象尺寸变化。

## 3. 后果

### 正面

- 共享语义成立：进程间 SHARED 映射彼此可见，不需要 unmap/remap。
- 稳态 4 KiB 数据区往返在首次 touch 之后没有每次请求的页表更新，也没有载荷
  拷贝；内核视图与用户映射同帧，`MemoryObject.read/write` 不再是唯一数据通路。
- 拆除 SHARED 映射不再有回写代价。

### 代价与限制

- 首次建立页缓存时对象被额外拷贝一次，并且驻留内存从逻辑尺寸上取整到页边界。
  对 16 MiB 上限内的对象这是一次性成本。
- 对象页缓存按页搬运（`read_locked`/`write_locked` 会跨页分段），因此跨页
  `read`/`write` 是逐页 `copy_bytes`，不再是单次整段拷贝。
- 不支持 `File.materialize` 之外的"SHARED 映射把写入持久化回文件"语义：
  SHARED 只作用于 MemoryObject 的页缓存。
- 不为 `pages_shared` 映射实现 COW，因此其写入权限在 `fork` 后仍然保留；
  需要 COW 的场景必须使用私有映射。

## 4. 验收

内核侧回归：

```bash
cmake --build build-debug -j
cmake --build build-debug --target make-root-image -j
ctest --test-dir build-debug -R "kernel_mobj_exec_test|naos_ipc_core" --output-on-failure
ctest --test-dir build-debug -R "boot_smoke_archive|boot_smoke_userland_services" --output-on-failure
```

共享语义（opt-in，`NAOS_MOBJ_SHARE_TEST=ON`）：

```bash
cmake -S . -B build-debug -DNAOS_MOBJ_SHARE_TEST=ON
ctest --test-dir build-debug -R boot_smoke_mobj_share --output-on-failure
```

该用例覆盖本 ADR 的六项断言（别名、稳定态写穿、每 `(对象, 页)` 单帧、私有映射
隔离、fork 隔离、只读准入 + 只读映射不发布写入）。`tests/mobj_share_init.sh`
在拿到 smoke 状态后调用 `/bin/poweroff`：**退出机器是启动驱动方的决定，不是测试
二进制的职责**，所以 smoke 只返回状态，由 opt-in 脚本结束这次启动。这样验证不再
依赖启动器的挂钟上限（实测 ~90s 降到 ~38s）。

`/bin/poweroff` 是 `nanobox` 的一个 applet，直接调用 `NA_SYSCALL_POWER_OFF`
（kernel `syscall/power.cc` → `arch::ACPI::shutdown()`，即 ACPI S5，与电源键同一
路径）。它不是特权操作：NaOS 没有启动权威 capability，该调用只能造成可用性损失。
`util/ln.sh` 建立 `poweroff` 符号链接，`util/make_root_image.py` 的
`BOOT_APPLET_ALIASES` 把符号链接实体化为普通文件（FAT 无符号链接）。

只读映射的**页表 fault 见证**（`--mobj-protect`）会故意触发一次用户态
SIGSEGV，内核会把它记成 fault 行，因此不能挂在 boot 断言下；单独执行并读取
内核日志：

```bash
python3 util/run.py --build-dir build-debug --init-script tests/mobj_protect_init.sh \
    q --iso -n --no-reboot
grep "exception 14\|mobj-protect" build-debug/kernel_out.log
```

期望：`exception 14 occurred at ... pid N` 之后紧跟
`mobj-protect: PASS denied=1 witness=1 unchanged=1`。
