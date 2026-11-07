#pragma once

#include "kernel/acpi/fixed_inventory.hpp"
#include "kernel/acpi/global_lock.hpp"
#include "kernel/acpi/sci_lifecycle.hpp"
#include "kernel/acpi/sci_owner.hpp"
#include "kernel/acpi/sci_status.hpp"
#include "kernel/acpi/transition_lifecycle.hpp"
#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/irq.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mutex.hpp"
#include "kernel/pci/topology_lifecycle.hpp"
#include "kernel/wait.hpp"
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE)
#include "kernel/timer.hpp"
#endif
#include <atomic>

namespace task
{
struct process_t;
}

namespace acpi
{

enum class global_lock_event_enable_result : u8
{
    enabled,
    hardware_absent,
    io_error,
};

class fixed_hardware final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::acpi_fixed_hardware; }

    explicit fixed_hardware(const fixed_inventory &inventory);
    ~fixed_hardware() override;

    bool start_sci();
    void set_sci_gsi(u32 gsi) { inventory_.sci_gsi = gsi; }
    const fixed_inventory &inventory() const { return inventory_; }

    sci_operation_result claim_transition_owner(task::process_t *caller, u64 boot_epoch, u64 firmware_generation);
    sci_operation_result claim_sci(task::process_t *caller, u64 boot_epoch, u64 firmware_generation);
    void owner_exit(process_id owner);
    void release_global_lock_for_thread_exit(process_id owner, u64 thread_id);
    bool get_platform_lineage(pci::platform_generation_lineage &lineage) const;
    sci_operation_result report_sci_failure(task::process_t *caller, u64 boot_epoch, u64 firmware_generation);
    sci_operation_result drain_sci(task::process_t *caller, u64 sequence, u32 &flags, u64 &pm1_status,
                                   u64 &pm1_status_b, u64 &gpe0_status, u64 &gpe1_status);
    sci_operation_result clear_sci_status(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                          u64 sequence, const sci_status_masks &masks);
    sci_operation_result ack_and_rearm_sci(task::process_t *caller, u64 sequence, u32 &state, u32 &flags);
    u32 sci_state_value() const;
    sci_operation_result prepare_sleep(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                       u32 sleep_type_a, u32 sleep_type_b, u64 &ticket);
    sci_operation_result commit_sleep(task::process_t *caller, u64 boot_epoch, u64 firmware_generation, u64 ticket,
                                      u32 &outcome);
    sci_operation_result prepare_reset(task::process_t *caller, u64 boot_epoch, u64 firmware_generation, u64 &ticket);
    sci_operation_result commit_reset(task::process_t *caller, u64 boot_epoch, u64 firmware_generation, u64 ticket,
                                      u32 &outcome);
    sci_operation_result abort_transition(task::process_t *caller, u64 boot_epoch, u64 firmware_generation, u64 ticket);
    sci_operation_result set_gpe_mask(task::process_t *caller, u64 boot_epoch, u64 firmware_generation, u32 bank,
                                      u64 mask, bool enable);
    global_lock_operation_result acquire_global_lock(task::process_t *caller, task::thread_t *execution_thread,
                                                     u64 timeout_us);
    global_lock_operation_result release_global_lock(task::process_t *caller, task::thread_t *execution_thread);

    na_signal_t capability_signals() const override;
    u64 capability_state() const override;

  private:
    irq::request_result on_interrupt(const irq::interrupt_info *, u64) noexcept;

    static bool read_register(const fixed_register &register_info, u64 &value);
    static bool read_optional_register(const fixed_register &register_info, u64 &value);
    static bool write_register(const fixed_register &register_info, u64 value);
    global_lock_event_enable_result enable_global_lock_event();
    bool disable_global_lock_event();
    void mark_global_lock_event_lost();
    bool signal_global_lock_release();
    bool read_facs_global_lock(u32 &value) const;
    bool compare_exchange_facs_global_lock(u32 &expected, u32 desired) const;
    bool release_facs_global_lock(bool &notify_firmware) const;
    void wake_global_lock_waiters();
    sci_operation_result sci_owner_result_locked(process_id owner) const;
    sci_operation_result transition_owner_result_locked(process_id owner) const;
    static bool has_pending(u64 pm1_status, u64 pm1_status_b, u64 gpe0_status, u64 gpe1_status)
    {
        return pm1_status != 0 || pm1_status_b != 0 || gpe0_status != 0 || gpe1_status != 0;
    }

    fixed_inventory inventory_;
    irq::registration sci_registration_;
    mutable lock::spinlock_t lock_;
    lock::mutex_t transaction_lock_;
    lock::mutex_t pm1_control_update_lock_;
    fixed_hardware_owner_lifecycle transition_owner_;
    fixed_hardware_owner_lifecycle sci_owner_;
    sci_event_lifecycle sci_event_;
    sci_status_ledger sci_status_ledger_;
    transition_lifecycle transition_;
    task::wait_queue_t global_lock_wait_queue_;
    std::atomic<u64> global_lock_wake_sequence_{1};
    process_id global_lock_owner_pid_ = 0;
    u64 global_lock_owner_tid_ = 0;
    bool global_lock_acquire_pending_ = false;
    bool global_lock_hardware_owned_ = false;
    bool global_lock_event_available_ = false;
    bool global_lock_event_lost_ = false;
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE)
    bool arm_test_firmware_global_lock();
    void test_firmware_release_global_lock(timeclock::microsecond_t) noexcept;
    void test_raise_qemu_global_lock_sci(timeclock::microsecond_t) noexcept;
    bool raise_qemu_global_lock_status_for_test() noexcept;
    std::atomic_bool test_firmware_global_lock_active_{false};
    std::atomic_bool test_firmware_release_scheduled_{false};
    std::atomic_bool test_sci_delivery_scheduled_{false};
    std::atomic_bool test_global_lock_waiter_blocked_{false};
    std::atomic<u32> test_firmware_release_attempts_{0};
#endif
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
    std::atomic_bool test_global_lock_release_failure_pending_{true};
#endif
};

} // namespace acpi
