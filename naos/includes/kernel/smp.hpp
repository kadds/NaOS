#pragma once
#include "cpu.hpp"
#include "kernel/common.hpp"
namespace SMP
{

void init();

/// tlb shutdown
void flush_all_tlb();

void reschedule_cpu(u32 cpuid);

/// call per cpu function
void call_cpu(u32 cpuid, cpu::call_cpu_func_t, u64 user_data);

/// Synchronize with the LAPIC that owns an MSI vector. The drain IPI has a
/// lower priority class than every allocatable MSI vector, and its handler
/// acknowledges only after the target has serviced any pending instance.
/// Returns false when the target/vector cannot participate; callers must
/// quarantine route resources on failure.
bool drain_msi_vector(u32 cpuid, u8 vector);

/// Called by the architecture interrupt exit path after EOI for the reserved
/// drain IPI. Not a general per-CPU callback interface.
void handle_interrupt_drain_ipi();

} // namespace SMP
