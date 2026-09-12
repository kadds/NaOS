# 数据面测量记录（W7）

本文件记录用户态数据面 ADR 的测量方法、原始输出与 gating 结论。它是 W1/W2 之后
的数据面基线，也是"是否把 libipc SPSC 环接入服务数据面"（W6 接入）的判据。

## 1. 测量范围与边界

被测面：**Linux UDS 主机传输**（`servicekit` 的 Linux 适配 + 真实守护进程
`ramdiskd`），即与平台无关的那部分数据面路径。

```bash
cargo build --locked --package ramdiskd --package naos-host-smoke --bins \
    --target x86_64-unknown-linux-gnu --target-dir build/host-services-target
./build/host-services-target/x86_64-unknown-linux-gnu/debug/naos-host-smoke measure \
    ./build/host-services-target/x86_64-unknown-linux-gnu/debug/ramdiskd
```

测量器在 `tests/host-smoke/src/main.rs` 的 `mod measure`，随既有 host 测试树一起
维护；它不读源码、不依赖 QEMU。

**明确不在本测量范围内**（不得据此声称收益）：

- 内核内的 payload 拷贝（submit 快照 / receive 拷出 / reply 拷入 / take_result
  拷出）以及页表更新：这些只发生在 guest 内，主机 UDS 路径不经过它们。
- W1 的共享页收益：它是"消除缺页分配私有页 + 逐次拷贝"的结构性收益，只能由
  guest 内的功能用例证明（`boot_smoke_mobj_share`），无法用本主机测量器观测。

主机无 `strace`/`perf`，因此**不报告 syscall 次数**；改用
`/proc/self/status` 的 context switch 计数，并在输出中写明这一点。

> **重要更正（2026-09-11）**：本文件初版给出的"QD 无扩展性"结论**是测量错误**，
> 不是系统属性。初版 harness 把一批 future 逐个 `await`（`for f in pending { f.await }`），
> 而未 poll 的 `async fn` future 推入 `Vec` 不会启动任何工作 —— 所以"深度"批次
> 实际是严格串行执行，每个深度都测到同一件事。修正为真正并发发出
> （`tokio::task::JoinSet` + 8 个 worker 线程）后，QD 出现明确扩展：
> 控制路径 2.6x、64 KiB 数据路径 1.6x。下文为修正后的数据与结论。

## 2. 原始输出（2026-09-11，`build/host-services-target`，修正 harness 后）

```text
measure: transport=linux-uds transfer_bytes=65536
measure: medium blocks=69632 logical=512 max_transfer_blocks=128 max_transfer_bytes=65536 max_in_flight=1
measure: qd1_control_latency p50=236us p95=291us p99=381us min=196us max=524us n=500
measure: qd=1 requests=256 total=148694us per_request=580.8us throughput=107.6MiB/s ctxt_switches=567 ctxt_per_request=2.21
measure: qd=8 requests=256 total=97855us per_request=382.2us throughput=163.5MiB/s ctxt_switches=554 ctxt_per_request=2.16
measure: qd=32 requests=256 total=93677us per_request=365.9us throughput=170.8MiB/s ctxt_switches=555 ctxt_per_request=2.17
measure: qd=128 requests=1024 total=366673us per_request=358.1us throughput=174.5MiB/s ctxt_switches=2176 ctxt_per_request=2.12
measure: control_qd=1 per_request=296.9us requests_per_s=3368
measure: control_qd=8 per_request=132.5us requests_per_s=7547
measure: control_qd=32 per_request=113.8us requests_per_s=8789
measure: segments_read_64k per_call: create=6us fill=0us register=179us submit=547us drain=6us
measure: segments_write_64k per_call: create=8us fill=21us register=187us submit=550us drain=0us
measure: size=4096B per_request=555.2us payload_MiB_per_s=7.0
measure: size=65536B per_request=548.4us payload_MiB_per_s=114.0
```

深度扫描必须用 `JoinSet` 真正并发发出；逐个 `await` 得到的是串行结果（见文首更正）。

## 3. 由数据得到的结论

1. **服务端读并发生效，且在设备广告的深度内线性获益**（见 §2.1）。`ramdiskd` 现在
   按到达顺序取锁：读请求取共享锁并把执行体交给独立任务，变更请求取独占锁并等待
   已在跑的读 —— 因此读可重叠、写是它前后请求的栅栏，block 排序语义不变。
2. **`max_in_flight` = 4**：广告值与准入界是同一个常量，设备说 4 就真的给 4
   （此前是 1，即并发读会被 `EAGAIN` 拒绝）。
3. **QD=1 有固定开销**：约 +2.5%（595.8us vs 串行基线 580.8us），即锁 + 任务 spawn
   的代价，此时无重叠可换。深度≥2 后转为净收益。
4. **开销与载荷大小相关性弱**：4 KiB 请求 555us，64 KiB 请求 548us —— 数据量放大
   16 倍，单次耗时几乎不变。单次成本是"每次调用"的固定开销，不是"每字节"。
5. **数据搬运只占小头**：64 KiB read 的分段耗时里 `create=6us` + `drain=6us`
   （write 的 `fill=21us`）共约 12~25us；`register≈180us` 与 `submit≈550us` 是主体。
6. **连接建立不是杠杆**：裸 `connect+close` 实测仅 **13.5us**（占 450us 的 3%）。
   "复用连接"不是有价值的优化方向，已明确不做。
7. **把响应写入移出关键路径会退化**：应答再 spawn 一层后单请求由 449us 升到 506us
   （-11%）。已回滚。现在读的执行体**自己**写响应（`Request::respond` 取 `self`），
   不额外 spawn。

### 2.1 并发读的实测（`build/host-services-target`，三次运行）

| 数据路径 | QD=1 | QD=2 | QD=4（= 广告上限） |
| --- | --- | --- | --- |
| 串行基线（`MAX_IN_FLIGHT=1`） | 580.8us / 107.6 MiB/s | 拒绝（EAGAIN） | 拒绝（EAGAIN） |
| 现在（读并发，`MAX_IN_FLIGHT=4`） | 595.8us / 104.9 | 418.3 / 149.4 | **348.4 / 179.4** |
| 复算（3 次运行区间） | 592~605us | 418~430us | 348~384us / 163~179 MiB/s |

深度内的收益：吞吐 **+71%**、单请求成本 **-42%**。

### 2.2 写路径并发（flush 等待变活）实测

写不再内联持锁完成，而是：校验准入（共享锁）→ 任务内等"最老未完成"闸门 → 独占锁应用 →
释锁后记账并信号；`flush` 在**不持介质锁**的情况下等待所有更早序号的写。

| 写路径 | QD=1 | QD=2 | QD=4（= 广告上限） |
| --- | --- | --- | --- |
| 本改动前 | 仅 QD=1（`MAX_IN_FLIGHT=1`，QD>1 被 `EAGAIN` 拒绝） | 拒绝 | 拒绝 |
| 本改动后 | 634.0us / 98.6 MiB/s | 474.0 / 131.8 | **405.6 / 154.1** |

深度内吞吐 **+56%**、单请求成本 **-36%**。单写延迟未退化：`segments_write_64k submit=560us`
（改动前同项记录 480~550us，运行间噪声范围内）。

读路径不受影响：读不触碰排序状态，QD=4 实测 365~392us / 159~171 MiB/s，与改动前
348~384us / 163~179 MiB/s 区间重叠。

**运行间噪声**：QD=1 的读/控制指标在相同二进制上实测波动可达 ±15%（643.7/643.7/742.4us），
因此单次对照不足以声称差异，改动效果以深度内（QD=2/4）的一致性差异为准。

### 3.1 已尝试并回滚的方案（含数据）

| 方案 | QD=1 数据 | QD=8 数据 | 结论 |
| --- | --- | --- | --- |
| 响应 spawn 成独立任务 | 534.7us（更差） | 503.1us（更差） | 回滚 |
| 读共享锁 + 变更 drain + `MAX_IN_FLIGHT=8` + 循环内 `select!` | 634.4us | 354.1us | 回滚 |
| 读共享锁 + 变更等锁 + 执行体自写响应 + `MAX_IN_FLIGHT=4` | 595.8us | — | **采用**（§2.1） |

前两者失败的原因都是**为重叠付出了比自己消除的等待更多的机器开销**：`select!` 让循环
每轮多一次就绪竞速，把响应写交给第三个任务又加一次调度。第三版保留了"取锁顺序 =
到达顺序"这唯一必要的机制，并让执行体自己负责应答，才实现净收益。

## 4. gating 结论（G3）：不进入 W6 接入

依据：

- SPSC 环的收益模型是**消除拷贝**，而拷贝实测只占单次 64 KiB 调用的 2~3%
  （`create`+`drain`+`fill`）。即使拷贝降到 0，第 2 节的数字也不会移动。
- 第 2.1/2.2 节表明吞吐杠杆是**管道深度**（客户端并发发起 + 服务端受理与执行重叠），
  而不是拷贝：同样 64 KiB 请求在 QD=4 下读达 ~171 MiB/s、写达 ~154 MiB/s，而 QD=1 的
  拷贝成本完全相同。
- 因此顺序是"先把深度用起来，再评估任何 transport 变更"。在深度未被利用前接入环，
  等于在一个未被填满的管道前端换更快的管子。

`naos/libipc` 的 SPSC 环作为**可选 transport** 保留并通过契约测试
（`naos_ipc_core_ring_test`、`naos_ipc_core_ring_c_api_test`、
`naos_ipc_core_transport_contract_test`）；它没有替换 `ClientEnd`，也没有被任何
服务接入。

重新评估的前置条件：

1. 深度已用满而吞吐仍不达标（即 QD 已饱和，需要更深队列、分片锁或按 LBA 分区的并发），或
2. 端到端成本中拷贝/页表占比上升到主导（用同一测量器复测即可判定）。

## 5. 已测量 vs 仅结构性验证的收益

| 项 | 状态 | 证据 |
| --- | --- | --- |
| 主机 UDS 数据面基线（QD/尺寸/分段/上下文切换） | **已测量** | §2 原始输出 |
| exfatd 由"每次 I/O 建 region"改为复用单一 region | **结构性验证**（未做吞吐 A/B） | `servicekit` 的 `reused_region_keeps_memory_create_and_mapping_counts_flat` 断言稳态下 region 创建数不随操作数增长；exfatd `RemoteBlockIo` 持有 `Arc<MemoryObject>` 单一 region |
| W1 共享页：消除每次缺页的私有页分配与载荷拷贝 | **guest 功能验证，未测吞吐** | `boot_smoke_mobj_share` 六项断言（别名/稳态写穿/单帧/私有隔离/fork 隔离/只读准入）通过；主机测量器不经过该代码，故不声称吞吐收益 |
| W2 bulk 契约等价（合法/缺权/方向/越界） | **已测（单元）** | `servicekit::memory::region_contract_tests` 三项用例 |
| W5 admission 上界与背压 | **已测（单元）+ guest 观测** | `servicekit::admission` 六项用例；`ramdiskd` 启动日志 `block service admission bound=4 in_flight=0 refused=0` |
| W5 服务端读并发 | **已测量**（§2.1） | QD=4 吞吐 179.4 MiB/s vs QD=1 104.9，单请求成本 -42%；`MAX_IN_FLIGHT=4` 是广告值与准入界的同一常量 |
| W5 flush 排序域（ADR §6.1） | **已测量 + 契约符合性** | `ramdiskd::ordering::FlushDomain`：按设备分配 admission 序号、写从准入跟踪到完成点、`flush` 只在其序号之前全部写到达后才完成、失败的写把该 errno 留给后续所有 flush。12 个纯状态机用例 + 4 个**计时断言**用例证明等待真实发生（写未落地时 flush 阻塞 ≥40ms 再满足）。 |
| W5 写路径并发（flush 等待变活） | **已测量**（§2.2） | 写按"最老未完成"闸门并发应用，排序交给账本：QD=4 吞吐 154.1 MiB/s vs QD=1 98.6（**+56%**），单写延迟未退化（`submit=560us` vs 改动前 480~550us） |
| init 启动时间线 | **已测量**（单次归因，非 A/B） | 根路由就绪→shell 6.38s → 5.57s：跳过空 `/etc/init.sh` 的 spawn（-0.89s）、startup barrier 改为真实 `open` 就绪探测（-0.24s）、删除重复 priming 循环 |

## 6. 复现与注意事项

- 测量会启动真实 `ramdiskd` 进程并在临时目录建立 UDS；运行前确认没有并发的
  QEMU/host-smoke 正在使用同一服务目录。
- 每次请求必须重新 `register`：Linux 传输在响应路径释放 region（这是其契约），
  QD>1 的批次需要 D 次注册。测量器按此建模，未绕开该契约。
- `tests/host-smoke` 的 `measure` 模式与既有 `smoke` 模式共用二进制；默认模式
  （无参数或非 `measure`）行为不变。
