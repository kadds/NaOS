# ADR：KVM pvclock 与 `event_clock` / `event_source` 计时架构

- 状态：Accepted（已实现）
- 日期：2026-09-16
- 范围：NaOS x86-64 内核

## 背景

NaOS 原有的 `clock_source` / `clock_event` 模型把“读取单调时间”和“产生定时器中断”
绑定在一起。计时器先选择 HPET、ACPI PM 或 PIT 作为全局源，再校准 TSC 和 Local APIC；
watcher 只能依赖固定周期 tick 被处理。这种设计使 KVM 来宾必须经过模拟平台计时器，且
无法独立选择更适合虚拟机的时间线和事件设备。

KVM pvclock 只提供每 vCPU 的单调时间换算参数，不产生中断。因此它应当与负责送达 deadline
的 Local APIC one-shot timer 组合使用，而不能替代事件源。

## 决策

### 1. 拆分时间线和事件源

新增两个互不持有对方的接口：

- `timeclock::event_clock`：无锁、可在 IRQ 上下文读取的纳秒单调时间线。
- `timeclock::event_source`：在本 CPU 为绝对 deadline 编程一次定时事件。

`timer` 核心拥有 watcher 队列和 deadline 的权威时间判断。事件源只接收带有时间观测值和
generation 的 `deadline_request`，硬件 IRQ 只投递软定时器中断；软中断重新读取
`event_clock::now_ns()`，一次性处理所有已到期 watcher，并重新编程下一次 deadline。

这样可以组合 `kvm-pvclock + local-apic`，也可以在裸机、TCG 或 KVM feature 被屏蔽时使用
TSC、HPET、ACPI PM 或 PIT 回退路径。

### 2. KVM pvclock 作为虚拟机首选时间线

仅在以下条件均满足时启用 pvclock：

1. `CPUID.1:ECX[31]` 宣布 hypervisor；
2. KVM vendor leaf 的签名为 `KVMKVMKVM`，且覆盖 `0x40000001`；
3. `CPUID.0x40000001:EAX[3]`（`CLOCKSOURCE2`）存在；
4. 当前 CPU 成功注册自己的 pvclock 页并读取合法 sample。

每个 CPU 分配独立的、清零的内核页，并通过 `MSR_KVM_SYSTEM_TIME_NEW` 写入
`GPA | 1`。停止时先写入 0，再释放该页；页不映射给用户态，也不跨 CPU 复用。旧 KVM
MSR、wall-clock、steal time 和其他 hypervisor 时钟不在本决策范围内。

pvclock 页使用 KVM 定义的 packed 32-byte ABI。读取采用有限 seqlock 重试；合法的
`tsc_shift`、固定点乘法、移位和最终加法均检查溢出。重试耗尽时使用最后一组合法换算参数，
结果通过 `last_returned_ns` 做单调 clamp，并记录健康计数，不让 scheduler、futex 或 IPC
deadline 看到时间倒退。

通过 `kvmclock=auto|on|off` 控制策略：

- `auto`：pvclock 不可用时回退到非 PV 时钟；
- `on`：pvclock 前置条件或首次 sample 失败时停止启动；
- `off`：不探测、不写 KVM MSR，强制走非 PV 路径。

pvclock 的 stable bit 只有在对应 CPUID 能力存在时才用于声明跨 CPU 单调性；否则只保证
本 CPU 时间线。BSP 选定时间线后，AP 必须注册自己的 pvclock 页并成功取得 sample，不能
在同一 SMP 系统中静默混用不具备跨 CPU 保证的时钟。

### 3. Local APIC 使用 one-shot deadline

Local APIC 作为独立的 `event_source`，以校准后的总线频率将 `deadline - now` 向上取整为
硬件 ticks，最短为一个 tick；超出 32-bit counter 的 deadline 分段编程。空 watcher 时
取消并屏蔽定时器。

每次 rearm/cancel 都推进 generation。旧中断即使到达，也会在 timer 核心的时间和 generation
检查中失效。callback 在锁外执行，并在调用前从队列删除，以保证取消竞态下每个 watcher 最多
执行一次。

### 4. 可观测性与验证边界

记录 stable sample、seqlock retry、缓存 fallback、倒退 clamp、非法 sample、暂停 sample、
late deadline、AP 注册失败、stale interrupt 和 source arm 失败；计数饱和而不回绕，异常告警
限频。启动日志包含选中的 `event_clock`、`event_source`、KVM feature 结果及 timer 健康摘要，
不打印 pvclock 页 GPA。

普通 `util/run.py` 只负责构建目录内镜像准备、QEMU 生命周期和退出状态。需要判断启动结果的
测试专用 wrapper 才能读取对应 build 目录的 `kernel_out.log` 并检查 marker。

## 后果

### 正面影响

- KVM 来宾可直接使用 pvclock 时间线，避免依赖 PIT/HPET/ACPI PM 校准。
- 时间读取和事件送达可独立演进；Local APIC one-shot 不再要求绑定某个 clock。
- timer 核心以纳秒保存 deadline，公共 microsecond API 只在边界转换并检查溢出。
- 暂停、迁移、合并或延迟的中断不会被误解为丢失周期；软中断始终依据当前绝对时间排空 watcher。
- 非 KVM 和 feature 被屏蔽的机器仍保留 TSC/platform 回退路径。

### 成本与限制

- 每 CPU 需要维护 pvclock 页和 MSR 生命周期；暂不支持 CPU hotplug。
- pvclock 和 Local APIC 的健康状态需要单独诊断，事件延迟仍不提供硬实时保证。
- 不具跨 CPU 单调保证的 pvclock 不能用于跨 CPU 绝对时间排序。
- one-shot deadline、长暂停和 stale interrupt 的正确性依赖 generation 与软中断重新判定。

## 实现记录

以下组件已迁移或新增：

- `naos/includes/kernel/time/event_clock.hpp`、`event_source.hpp`、`unsigned_math.hpp`；
- KVM pvclock probe、ABI、每 CPU 注册、seqlock 读取和 fixed-point conversion；
- CPU 信息与 ordered TSC helper；
- TSC、Local APIC、PIT、HPET、ACPI PM 的 `event_clock` / `event_source` 适配器；
- timer 核心的纳秒 deadline、earliest-deadline rearm、generation 和诊断计数；
- pvclock、无符号算术、deadline conversion 以及 KVM boot smoke 测试。

旧的 `naos/includes/kernel/clock/clock_event.hpp` 和 `clock_source.hpp` 已删除，避免新的
实现继续依赖双向关联接口。

## 验证记录

应在提交或发布前执行：

```bash
cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure -R 'pvclock|timer|wait_deadline'
```

在 `/dev/kvm` 可用的主机上，使用测试专用 wrapper 通过现有 launcher 启动 ISO，并在所选
build 目录的 `kernel_out.log` 中确认：

- `event-clock=kvm-pvclock`；
- `event-source=local-apic`；
- 正常服务启动 marker；
- 多 vCPU AP 的 pvclock 注册日志。

还应验证 `kvmclock=off` 的非 PV 启动、无 KVM/TCG 回退，以及 feature 被屏蔽时
`kvmclock=on` 的可辨识失败路径。

初始 KVM boot smoke 曾在多个并发 `EPOLL_CTL_ADD` 共同准备 ready ring 容量时触发
`naos/src/kernel/ipc/epoll.cc:104` 的 `ready_count_ < size` 断言；修复后通过让容量准备与
registration commit 由同一把分配锁串行化消除了该竞态。修复后的验证结果为：
`cmake --build build-kvm-pvclock -j`、pvclock/event-source 单元测试和 KVM boot smoke 均通过，
日志包含 `event-clock=kvm-pvclock`、`event-source=local-apic`、正常服务启动及
`init: user shell started`。

## 参考资料

- [Linux timekeeping](https://www.kernel.org/doc/html/latest/timers/timekeeping.html)
- [KVM x86 timekeeping](https://docs.kernel.org/virt/kvm/x86/timekeeping.html)
- [KVM x86 CPUID](https://www.kernel.org/doc/html/latest/virt/kvm/x86/cpuid.html)
- [KVM x86 MSR](https://www.kernel.org/doc/html/latest/virt/kvm/x86/msr.html)
- [QEMU KVM PV](https://www.qemu.org/docs/master/system/i386/kvm-pv.html)
