#include "kernel/acpi/fixed_hardware.hpp"
#include "kernel/acpi/event_enable.hpp"

#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include <atomic>
#include <limits>

KLOG_MODULE(acpi);

namespace acpi
{
namespace
{
bool is_live_caller(const task::process_t *caller)
{
    return caller != nullptr && caller->pid != 0 &&
           (caller->attributes.load(std::memory_order_acquire) & task::process_attributes::exiting) == 0;
}

struct transition_clock_sample
{
    u64 now_ns = 0;
    bool cross_cpu_monotonic = false;
};

transition_clock_sample sample_transition_clock() noexcept
{
    const bool cross_cpu_monotonic = timer::high_resolution_time_is_cross_cpu_monotonic();
    return {cross_cpu_monotonic ? timer::get_high_resolution_time_ns() : 0, cross_cpu_monotonic};
}

bool read_register_scalar(const fixed_register &register_info, u32 offset, u8 access_width, u64 &value)
{
    const u64 address = register_info.address + offset;
    if (address % access_width != 0)
        return false;
    if (register_info.space == 1)
    {
        const auto port = static_cast<io_port>(address);
        switch (access_width)
        {
            case 1:
                value = io_in8(port);
                return true;
            case 2:
                value = io_in16(port);
                return true;
            case 4:
                value = io_in32(port);
                return true;
            default:
                return false;
        }
    }

    auto *base = memory::pa2va<byte *>(phy_addr_t::from(address));
    if (base == nullptr)
        return false;
    switch (access_width)
    {
        case 1:
            value = *reinterpret_cast<volatile const u8 *>(base);
            return true;
        case 2:
            value = *reinterpret_cast<volatile const u16 *>(base);
            return true;
        case 4:
            value = *reinterpret_cast<volatile const u32 *>(base);
            return true;
        case 8:
            value = *reinterpret_cast<volatile const u64 *>(base);
            return true;
        default:
            return false;
    }
}

bool write_register_scalar(const fixed_register &register_info, u32 offset, u8 access_width, u64 value)
{
    const u64 address = register_info.address + offset;
    if (address % access_width != 0)
        return false;
    if (register_info.space == 1)
    {
        const auto port = static_cast<io_port>(address);
        switch (access_width)
        {
            case 1:
                io_out8(port, static_cast<u8>(value));
                return true;
            case 2:
                io_out16(port, static_cast<u16>(value));
                return true;
            case 4:
                io_out32(port, static_cast<u32>(value));
                return true;
            default:
                return false;
        }
    }

    auto *base = memory::pa2va<byte *>(phy_addr_t::from(address));
    if (base == nullptr)
        return false;
    switch (access_width)
    {
        case 1:
            *reinterpret_cast<volatile u8 *>(base) = static_cast<u8>(value);
            return true;
        case 2:
            *reinterpret_cast<volatile u16 *>(base) = static_cast<u16>(value);
            return true;
        case 4:
            *reinterpret_cast<volatile u32 *>(base) = static_cast<u32>(value);
            return true;
        case 8:
            *reinterpret_cast<volatile u64 *>(base) = value;
            return true;
        default:
            return false;
    }
}

} // namespace

fixed_hardware::fixed_hardware(const fixed_inventory &inventory)
    : kobject(type_of())
    , inventory_(inventory)
{
}

fixed_hardware::~fixed_hardware()
{
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (sci_event_.claimed() && sci_event_.state() == sci_event_state::unmasked)
            (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
        sci_event_.degrade();
    }
    (void)disable_global_lock_event();
    (void)disable_acpi_event_sources(inventory_, write_register);
    sci_registration_.reset();
}

bool fixed_hardware::start_sci()
{
    if (sci_registration_)
        return true;
    if (inventory_.sci_gsi > 255)
    {
        mark_global_lock_event_lost();
        return false;
    }
    // The source must be masked before the handler becomes reachable. The
    // firmware may leave the legacy controller enabled across boot, and an
    // unclaimed level-sensitive SCI cannot be safely drained by the kernel.
    if (!arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi)))
    {
        mark_global_lock_event_lost();
        return false;
    }
    if (!disable_acpi_event_sources(inventory_, write_register))
    {
        mark_global_lock_event_lost();
        return false;
    }
    const u8 vector = arch::APIC::sci_vector();
    if (vector == 0)
    {
        mark_global_lock_event_lost();
        return false;
    }
    sci_registration_ = irq::register_handler(vector, irq::hard_handler::bind<&fixed_hardware::on_interrupt>(*this));
    if (!sci_registration_)
    {
        mark_global_lock_event_lost();
        return false;
    }

    const auto event_result = inventory_.facs_global_lock_address != 0 && !inventory_.hardware_reduced
                                  ? enable_global_lock_event()
                                  : global_lock_event_enable_result::hardware_absent;
    const bool global_lock_available = event_result == global_lock_event_enable_result::enabled;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const bool event_path_lost = global_lock_event_available_ && !global_lock_available;
        global_lock_event_available_ = global_lock_available;
        global_lock_event_lost_ = global_lock_event_lost_after_failure(event_path_lost, global_lock_event_lost_);
    }
    if (inventory_.facs_global_lock_address != 0 && !inventory_.hardware_reduced)
    {
        if (global_lock_available)
            KLOG_INFO("ACPI: FACS Global Lock SCI event enabled");
        else
            KLOG_INFO("ACPI: FACS Global Lock SCI event unavailable; using FACS polling");
    }
    else
        KLOG_INFO("ACPI: FACS Global Lock unavailable; using local software serialization");
    if (event_result == global_lock_event_enable_result::io_error)
    {
        wake_global_lock_waiters();
    }
    else if (global_lock_available && !arch::APIC::io_enable(static_cast<u8>(inventory_.sci_gsi)))
    {
        (void)disable_global_lock_event();
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            global_lock_event_available_ = false;
            global_lock_event_lost_ = true;
        }
        wake_global_lock_waiters();
        sci_registration_.reset();
        return false;
    }
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE) && !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT) &&                     \
    !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
    // The firmware actor exists only for the waiter-wake smoke. Owner-exit
    // and release-failure smokes acquire the free FACS lock directly.
    if (!arm_test_firmware_global_lock())
    {
        (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
        (void)disable_global_lock_event();
        mark_global_lock_event_lost();
        sci_registration_.reset();
        return false;
    }
#endif
    return true;
}

#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE)
bool fixed_hardware::arm_test_firmware_global_lock()
{
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!global_lock_event_available_ || global_lock_event_lost_)
            return false;
    }

    u32 current = 0;
    if (!read_facs_global_lock(current))
        return false;
    for (;;)
    {
        if ((current & facs_global_lock_owned) != 0)
            return false;
        u32 expected = current;
        const u32 desired = (current & ~facs_global_lock_pending) | facs_global_lock_owned;
        if (compare_exchange_facs_global_lock(expected, desired))
            break;
        current = expected;
    }

    test_firmware_global_lock_active_.store(true, std::memory_order_release);
    KLOG_INFO("ACPI TEST: firmware actor acquired FACS Global Lock");
    return true;
}

bool fixed_hardware::raise_qemu_global_lock_status_for_test() noexcept
{
    constexpr u64 qemu_test_status_offset_from_pm1_control = 0x3c;
    if (inventory_.pm1_control.space != 1 ||
        inventory_.pm1_control.address >
            static_cast<u64>(std::numeric_limits<u16>::max()) - qemu_test_status_offset_from_pm1_control)
        return false;

    const auto port = static_cast<io_port>(inventory_.pm1_control.address + qemu_test_status_offset_from_pm1_control);
    io_out8(port, 1);
    return true;
}

void fixed_hardware::test_firmware_release_global_lock(timeclock::microsecond_t) noexcept
{
    if (!test_firmware_global_lock_active_.load(std::memory_order_acquire))
        return;

    if (!test_global_lock_waiter_blocked_.load(std::memory_order_acquire))
    {
        constexpr u32 max_release_wait_attempts = 1'000;
        const u32 attempt = test_firmware_release_attempts_.fetch_add(1, std::memory_order_acq_rel);
        if (attempt < max_release_wait_attempts &&
            timer::schedule_after(
                10'000, timer::timer_handler::bind<&fixed_hardware::test_firmware_release_global_lock>(*this)) !=
                timer::invalid_watcher_id)
            return;

        test_firmware_global_lock_active_.store(false, std::memory_order_release);
        mark_global_lock_event_lost();
        wake_global_lock_waiters();
        KLOG_ERROR("ACPI TEST: firmware actor release fired without a blocked Global Lock waiter");
        return;
    }
    if (!test_firmware_global_lock_active_.exchange(false, std::memory_order_acq_rel))
        return;

    bool notify_firmware = false;
    if (!release_facs_global_lock(notify_firmware) || !notify_firmware)
    {
        mark_global_lock_event_lost();
        wake_global_lock_waiters();
        KLOG_ERROR("ACPI TEST: firmware actor could not release a pending FACS Global Lock");
        return;
    }

    if (!raise_qemu_global_lock_status_for_test())
    {
        mark_global_lock_event_lost();
        wake_global_lock_waiters();
        KLOG_ERROR("ACPI TEST: could not request QEMU PM1 GBL_STS event for blocked waiter");
        return;
    }
    KLOG_INFO("ACPI TEST: QEMU PM1 GBL_STS event requested for blocked waiter");
}

void fixed_hardware::test_raise_qemu_global_lock_sci(timeclock::microsecond_t) noexcept
{
    if (!raise_qemu_global_lock_status_for_test())
    {
        mark_global_lock_event_lost();
        KLOG_ERROR("ACPI TEST: could not request QEMU PM1 GBL_STS event for claimed SCI owner");
        return;
    }
    KLOG_INFO("ACPI TEST: QEMU PM1 GBL_STS event requested for claimed SCI owner");
}
#endif

global_lock_event_enable_result fixed_hardware::enable_global_lock_event()
{
    enum class register_result : u8
    {
        enabled,
        absent,
        io_error,
    };

    const auto update = [](const fixed_register &reg, bool required) {
        if (reg.address == 0 && reg.length == 0)
            return required ? register_result::absent : register_result::enabled;
        if (reg.address == 0 || reg.length == 0 || reg.length > sizeof(u64) || reg.length * 8 <= 5)
            return register_result::io_error;
        u64 current = 0;
        if (!fixed_hardware::read_register(reg, current))
            return register_result::io_error;
        const u64 next = current | pm1_global_lock_enable;
        if (!fixed_hardware::write_register(reg, next))
            return register_result::io_error;
        u64 observed = 0;
        if (!fixed_hardware::read_register(reg, observed))
            return register_result::io_error;
        return (observed & pm1_global_lock_enable) != 0 ? register_result::enabled : register_result::absent;
    };

    const auto primary = update(inventory_.pm1_event_enable, true);
    const auto secondary = update(inventory_.pm1_event_b_enable, false);
    if (primary == register_result::io_error || secondary == register_result::io_error)
    {
        (void)disable_global_lock_event();
        return global_lock_event_enable_result::io_error;
    }
    if (primary == register_result::absent || secondary == register_result::absent)
    {
        if (!disable_global_lock_event())
            return global_lock_event_enable_result::io_error;
        return global_lock_event_enable_result::hardware_absent;
    }
    return global_lock_event_enable_result::enabled;
}

bool fixed_hardware::disable_global_lock_event()
{
    const auto update = [](const fixed_register &reg) {
        if (reg.address == 0 && reg.length == 0)
            return true;
        if (reg.address == 0 || reg.length == 0 || reg.length > sizeof(u64) || reg.length * 8 <= 5)
            return false;
        u64 current = 0;
        if (!fixed_hardware::read_register(reg, current))
            return false;
        return fixed_hardware::write_register(reg, current & ~u64(pm1_global_lock_enable));
    };
    return update(inventory_.pm1_event_enable) && update(inventory_.pm1_event_b_enable);
}

void fixed_hardware::mark_global_lock_event_lost()
{
    if (inventory_.facs_global_lock_address == 0 || inventory_.hardware_reduced)
        return;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        global_lock_event_lost_ =
            global_lock_event_lost_after_failure(global_lock_event_available_, global_lock_event_lost_);
        global_lock_event_available_ = false;
    }
    wake_global_lock_waiters();
}

bool fixed_hardware::signal_global_lock_release()
{
    const auto set_release = [](const fixed_register &reg) {
        if (reg.address == 0 && reg.length == 0)
            return true;
        if (reg.address == 0 || reg.length == 0 || reg.length > sizeof(u64) || reg.length * 8 <= 2)
            return false;
        u64 current = 0;
        if (!fixed_hardware::read_register(reg, current))
            return false;
        return fixed_hardware::write_register(reg, next_pm1_global_lock_release(current));
    };
    return with_pm1_control_update_lock<uctx::LockGuard_t<lock::mutex_t>>(pm1_control_update_lock_, [&] {
        return inventory_.pm1_control.address != 0 && set_release(inventory_.pm1_control) &&
               set_release(inventory_.pm1_control_b);
    });
}

bool fixed_hardware::read_facs_global_lock(u32 &value) const
{
    const u64 address = inventory_.facs_global_lock_address;
    if (address == 0 || (address & (alignof(u32) - 1)) != 0)
        return false;
    auto *word = memory::pa2va<u32 *>(phy_addr_t::from(address));
    if (word == nullptr)
        return false;
    value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    return true;
}

bool fixed_hardware::compare_exchange_facs_global_lock(u32 &expected, u32 desired) const
{
    const u64 address = inventory_.facs_global_lock_address;
    if (address == 0 || (address & (alignof(u32) - 1)) != 0)
        return false;
    auto *word = memory::pa2va<u32 *>(phy_addr_t::from(address));
    if (word == nullptr)
        return false;
    return __atomic_compare_exchange_n(word, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

bool fixed_hardware::release_facs_global_lock(bool &notify_firmware) const
{
    notify_firmware = false;
    u32 current = 0;
    if (!read_facs_global_lock(current))
        return false;
    for (;;)
    {
        const auto update = next_global_lock_release(current);
        if (!update.owned)
            return false;
        u32 expected = current;
        if (compare_exchange_facs_global_lock(expected, update.value))
        {
            notify_firmware = update.notify_firmware;
            return true;
        }
        current = expected;
    }
}

void fixed_hardware::wake_global_lock_waiters()
{
    global_lock_wake_sequence_.fetch_add(1, std::memory_order_release);
    global_lock_wait_queue_.do_wake_up();
}

sci_operation_result fixed_hardware::claim_transition_owner(task::process_t *caller, u64 boot_epoch,
                                                            u64 firmware_generation)
{
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
        return sci_operation_result::stale_generation;
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    return transition_owner_.claim(caller->pid) ? sci_operation_result::success
                                                : sci_operation_result::resource_revoked;
}

sci_operation_result fixed_hardware::claim_sci(task::process_t *caller, u64 boot_epoch, u64 firmware_generation)
{
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
        return sci_operation_result::stale_generation;
    pci::platform_generation_lineage lineage{};
    if (!get_platform_lineage(lineage))
        return sci_operation_result::not_supported;
    if (!pci::topology_lifetime().is_platform_generation_current(lineage))
        return sci_operation_result::resource_revoked;
    const auto owner = caller->pid;
    bool enable_failed = false;
    {
        uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
        if (!is_live_caller(caller))
            return sci_operation_result::access_denied;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            if (!sci_registration_)
                return sci_operation_result::not_supported;
            if (sci_event_.state() == sci_event_state::degraded)
                return sci_operation_result::resource_revoked;
            if (sci_event_.claimed())
                return sci_owner_.owns(owner) ? sci_operation_result::success : sci_operation_result::conflict;
        }
        if (!arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi)))
            return sci_operation_result::io_error;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            if (!sci_registration_)
                return sci_operation_result::not_supported;
            if (sci_event_.state() == sci_event_state::degraded)
                return sci_operation_result::resource_revoked;
            if (sci_event_.claimed())
                return sci_owner_.owns(owner) ? sci_operation_result::success : sci_operation_result::conflict;
            if (!sci_owner_.claim(owner))
                return sci_operation_result::resource_revoked;
            if (!sci_event_.claim_source())
            {
                (void)sci_owner_.owner_exit(owner);
                sci_event_.rearm_failed();
                enable_failed = true;
            }
        }
        if (!enable_failed && !disable_acpi_event_sources(inventory_, write_register))
        {
            {
                uctx::RawSpinLockUninterruptibleContext guard(lock_);
                (void)sci_owner_.owner_exit(owner);
                sci_event_.rearm_failed();
            }
            (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
            enable_failed = true;
        }
        if (!enable_failed)
        {
            const auto event_result = inventory_.facs_global_lock_address != 0 && !inventory_.hardware_reduced
                                          ? enable_global_lock_event()
                                          : global_lock_event_enable_result::hardware_absent;
            const bool global_lock_event_available = event_result == global_lock_event_enable_result::enabled;
            bool event_path_lost = false;
            {
                uctx::RawSpinLockUninterruptibleContext guard(lock_);
                event_path_lost = global_lock_event_available_ && !global_lock_event_available;
                global_lock_event_available_ = global_lock_event_available;
                global_lock_event_lost_ =
                    global_lock_event_lost_after_failure(event_path_lost, global_lock_event_lost_);
            }
            if (event_path_lost || event_result == global_lock_event_enable_result::io_error)
                wake_global_lock_waiters();
        }
        if (!enable_failed && !arch::APIC::io_enable(static_cast<u8>(inventory_.sci_gsi)))
        {
            {
                uctx::RawSpinLockUninterruptibleContext guard(lock_);
                (void)sci_owner_.owner_exit(owner);
                sci_event_.rearm_failed();
            }
            (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
            enable_failed = true;
        }
    }
    if (enable_failed)
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            global_lock_event_lost_ =
                global_lock_event_lost_after_failure(global_lock_event_available_, global_lock_event_lost_);
            global_lock_event_available_ = false;
        }
        wake_global_lock_waiters();
        notify_readiness();
    }
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE) && !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT) &&                     \
    !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
    if (!enable_failed && !test_sci_delivery_scheduled_.exchange(true, std::memory_order_acq_rel))
    {
        const auto watcher = timer::schedule_after(
            50'000, timer::timer_handler::bind<&fixed_hardware::test_raise_qemu_global_lock_sci>(*this));
        if (watcher == timer::invalid_watcher_id)
        {
            mark_global_lock_event_lost();
            return sci_operation_result::io_error;
        }
    }
#endif
    return enable_failed ? sci_operation_result::io_error : sci_operation_result::success;
}

bool fixed_hardware::get_platform_lineage(pci::platform_generation_lineage &lineage) const
{
    return pci::topology_lifetime().get_platform_generation_lineage(lineage) &&
           lineage.boot_epoch == inventory_.boot_epoch && lineage.firmware_generation == inventory_.firmware_generation;
}

sci_operation_result fixed_hardware::sci_owner_result_locked(process_id owner) const
{
    if (sci_owner_.owns(owner))
        return sci_operation_result::success;
    return sci_owner_.revoked_for(owner) ? sci_operation_result::resource_revoked : sci_operation_result::access_denied;
}

sci_operation_result fixed_hardware::transition_owner_result_locked(process_id owner) const
{
    if (transition_owner_.owns(owner))
        return sci_operation_result::success;
    return transition_owner_.revoked_for(owner) ? sci_operation_result::resource_revoked
                                                : sci_operation_result::access_denied;
}

void fixed_hardware::owner_exit(process_id owner)
{
    bool sci_revoked = false;
    bool transition_revoked = false;
    bool global_lock_owner_revoked = false;
    bool release_global_lock = false;
    {
        uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            sci_revoked = sci_owner_.owner_exit(owner);
            transition_revoked = transition_owner_.owner_exit(owner);
            if (global_lock_owner_pid_ == owner)
            {
                global_lock_owner_revoked = true;
                release_global_lock = global_lock_hardware_owned_;
                global_lock_owner_pid_ = 0;
                global_lock_owner_tid_ = 0;
                global_lock_acquire_pending_ = false;
                global_lock_hardware_owned_ = false;
            }
            if (!sci_revoked && !transition_revoked && !global_lock_owner_revoked)
                return;
            if (sci_revoked)
            {
                sci_event_.degrade();
                sci_status_ledger_.invalidate();
                global_lock_event_lost_ =
                    global_lock_event_lost_after_failure(global_lock_event_available_, global_lock_event_lost_);
                global_lock_event_available_ = false;
            }
            if (transition_revoked)
                transition_.owner_exit();
        }
        if (sci_revoked)
        {
            (void)pci::topology_lifetime().invalidate(inventory_.boot_epoch, inventory_.firmware_generation);
            // Sticky degradation leaves the level-sensitive source masked. A
            // restarted process may inspect the failure but cannot take over.
            (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
            (void)disable_acpi_event_sources(inventory_, write_register);
        }
        if (release_global_lock)
        {
            bool pending = false;
            if (!release_facs_global_lock(pending) || (pending && !signal_global_lock_release()))
            {
                {
                    uctx::RawSpinLockUninterruptibleContext guard(lock_);
                    global_lock_event_available_ = false;
                    global_lock_event_lost_ = true;
                }
                KLOG_ERROR("ACPI: failed to release FACS Global Lock during owner exit pid={}", owner);
            }
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT)
            else
            {
                u32 final_facs = 0;
                if (read_facs_global_lock(final_facs) &&
                    (final_facs & (facs_global_lock_owned | facs_global_lock_pending)) == 0)
                    KLOG_INFO("ACPI TEST: kernel released FACS Global Lock during process owner exit");
                else
                    KLOG_ERROR("ACPI TEST: FACS Global Lock remained owned or pending after process owner exit");
            }
#endif
        }
    }
    if (global_lock_owner_revoked || sci_revoked)
        wake_global_lock_waiters();
    notify_readiness();
}

void fixed_hardware::release_global_lock_for_thread_exit(process_id owner, u64 thread_id)
{
    bool release_global_lock = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (global_lock_owner_pid_ != owner || global_lock_owner_tid_ != thread_id)
            return;
        release_global_lock = global_lock_hardware_owned_;
        global_lock_owner_pid_ = 0;
        global_lock_owner_tid_ = 0;
        global_lock_acquire_pending_ = false;
        global_lock_hardware_owned_ = false;
    }
    if (release_global_lock)
    {
        bool pending = false;
        if (!release_facs_global_lock(pending) || (pending && !signal_global_lock_release()))
        {
            {
                uctx::RawSpinLockUninterruptibleContext guard(lock_);
                global_lock_event_lost_ = true;
                global_lock_event_available_ = false;
            }
            KLOG_ERROR("ACPI: failed to release FACS Global Lock during thread exit pid={} tid={}", owner, thread_id);
        }
    }
    wake_global_lock_waiters();
}

sci_operation_result fixed_hardware::report_sci_failure(task::process_t *caller, u64 boot_epoch,
                                                        u64 firmware_generation)
{
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto owner_result = sci_owner_result_locked(caller->pid);
        if (owner_result != sci_operation_result::success)
            return owner_result;
        if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
            return sci_operation_result::stale_generation;
        if (!sci_event_.claimed() || sci_event_.state() == sci_event_state::degraded)
            return sci_operation_result::resource_revoked;
        if (!sci_owner_.owner_exit(caller->pid))
            return sci_operation_result::resource_revoked;
        sci_event_.degrade();
        sci_status_ledger_.invalidate();
        global_lock_event_lost_ =
            global_lock_event_lost_after_failure(global_lock_event_available_, global_lock_event_lost_);
        global_lock_event_available_ = false;
    }
    (void)pci::topology_lifetime().invalidate(inventory_.boot_epoch, inventory_.firmware_generation);
    const bool source_masked = arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
    const bool event_sources_disabled = disable_acpi_event_sources(inventory_, write_register);
    wake_global_lock_waiters();
    notify_readiness();
    return source_masked && event_sources_disabled ? sci_operation_result::success : sci_operation_result::io_error;
}

irq::request_result fixed_hardware::on_interrupt(const irq::interrupt_info *, u64) noexcept
{
    u64 pm1_status = 0;
    u64 pm1_status_b = 0;
    if (!read_optional_register(inventory_.pm1_event, pm1_status) ||
        !read_optional_register(inventory_.pm1_event_b, pm1_status_b))
    {
        (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
        mark_global_lock_event_lost();
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            sci_event_.rearm_failed();
        }
        wake_global_lock_waiters();
        notify_readiness();
        return irq::request_result::no_handled;
    }
    bool global_lock_release_pending = ((pm1_status | pm1_status_b) & pm1_global_lock_status) != 0;
    if (global_lock_release_pending)
    {
        KLOG_INFO("ACPI: PM1 Global Lock status observed on SCI status_a={} status_b={}", log::hex(pm1_status),
                  log::hex(pm1_status_b));
        wake_global_lock_waiters();
    }

    sci_interrupt_action action = sci_interrupt_action::suppressed;
    bool sci_claimed = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        sci_claimed = sci_event_.claimed();
        if (sci_claimed && !transition_.committing())
            action = sci_event_.interrupt();
    }
    const bool source_masked = arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));

    if (!sci_claimed && global_lock_release_pending)
    {
        bool status_cleared = source_masked;
        if ((pm1_status & pm1_global_lock_status) != 0)
            status_cleared = write_register(inventory_.pm1_event, pm1_global_lock_status) && status_cleared;
        if ((pm1_status_b & pm1_global_lock_status) != 0)
            status_cleared = write_register(inventory_.pm1_event_b, pm1_global_lock_status) && status_cleared;
        if (status_cleared && arch::APIC::io_enable(static_cast<u8>(inventory_.sci_gsi)))
            return irq::request_result::ok;

        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            global_lock_event_available_ = false;
            global_lock_event_lost_ = true;
        }
        wake_global_lock_waiters();
        return irq::request_result::no_handled;
    }

    bool notify = action != sci_interrupt_action::suppressed;
    if (!source_masked)
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const bool newly_degraded = sci_event_.state() != sci_event_state::degraded;
        sci_event_.rearm_failed();
        notify = notify || newly_degraded;
    }
    if (notify)
        notify_readiness();
    return action == sci_interrupt_action::suppressed ? irq::request_result::no_handled : irq::request_result::ok;
}

sci_operation_result fixed_hardware::drain_sci(task::process_t *caller, u64 sequence, u32 &flags, u64 &pm1_status,
                                               u64 &pm1_status_b, u64 &gpe0_status, u64 &gpe1_status)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    const auto owner = caller->pid;
    const auto now_before_drain = sample_transition_clock();
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto owner_status = sci_owner_result_locked(owner);
        if (owner_status != sci_operation_result::success)
            return owner_status;
        if (transition_.active(now_before_drain.now_ns))
            return sci_operation_result::conflict;
        switch (sci_event_.begin_drain(sequence))
        {
            case sci_drain_result::ready:
                break;
            case sci_drain_result::would_block:
                return sci_operation_result::would_block;
            case sci_drain_result::stale_sequence:
                return sci_operation_result::stale_generation;
            case sci_drain_result::resource_revoked:
                return sci_operation_result::resource_revoked;
        }
    }

    if (!read_optional_register(inventory_.pm1_event, pm1_status) ||
        !read_optional_register(inventory_.pm1_event_b, pm1_status_b) ||
        !read_optional_register(inventory_.gpe0_status, gpe0_status) ||
        !read_optional_register(inventory_.gpe1_status, gpe1_status))
    {
        (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
        mark_global_lock_event_lost();
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            sci_event_.rearm_failed();
            sci_status_ledger_.invalidate();
        }
        wake_global_lock_waiters();
        notify_readiness();
        return sci_operation_result::io_error;
    }
    if (((pm1_status | pm1_status_b) & pm1_global_lock_status) != 0)
        wake_global_lock_waiters();

    const auto now_after_drain = sample_transition_clock();
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto owner_status = sci_owner_result_locked(owner);
        if (owner_status != sci_operation_result::success)
            return owner_status;
        if (transition_.active(now_after_drain.now_ns))
            return sci_operation_result::conflict;
        switch (sci_event_.finish_drain(sequence, flags))
        {
            case sci_drain_result::ready:
                break;
            case sci_drain_result::would_block:
                return sci_operation_result::would_block;
            case sci_drain_result::stale_sequence:
                return sci_operation_result::stale_generation;
            case sci_drain_result::resource_revoked:
                return sci_operation_result::resource_revoked;
        }
        if (!sci_status_ledger_.record(sequence, sci_status_masks{pm1_status, pm1_status_b, gpe0_status, gpe1_status}))
            return sci_operation_result::invalid_argument;
    }
    return sci_operation_result::success;
}

sci_operation_result fixed_hardware::clear_sci_status(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                                      u64 sequence, const sci_status_masks &masks)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    const auto now_before_clear = sample_transition_clock();

    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto owner_status = sci_owner_result_locked(caller->pid);
        if (owner_status != sci_operation_result::success)
            return owner_status;
        if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
            return sci_operation_result::stale_generation;
        if (!sci_event_.claimed() || sci_event_.state() == sci_event_state::degraded)
            return sci_operation_result::resource_revoked;
        if (sci_event_.state() != sci_event_state::masked)
            return sci_operation_result::not_ready;
        if (transition_.active(now_before_clear.now_ns))
            return sci_operation_result::conflict;

        switch (sci_status_ledger_.validate_clear(sequence, masks))
        {
            case sci_status_clear_error::none:
                break;
            case sci_status_clear_error::no_drain:
                return sci_operation_result::not_ready;
            case sci_status_clear_error::stale_sequence:
                return sci_operation_result::stale_generation;
            case sci_status_clear_error::empty:
                return sci_operation_result::invalid_argument;
            case sci_status_clear_error::unobserved_bits:
                return sci_operation_result::access_denied;
            case sci_status_clear_error::already_cleared:
                return sci_operation_result::conflict;
        }
    }

    const bool written = (masks.pm1_event == 0 || write_register(inventory_.pm1_event, masks.pm1_event)) &&
                         (masks.pm1_event_b == 0 || write_register(inventory_.pm1_event_b, masks.pm1_event_b)) &&
                         (masks.gpe0 == 0 || write_register(inventory_.gpe0_status, masks.gpe0)) &&
                         (masks.gpe1 == 0 || write_register(inventory_.gpe1_status, masks.gpe1));
    if (!written)
        return sci_operation_result::io_error;

    const auto now_after_clear = sample_transition_clock();
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (!sci_event_.claimed() || sci_event_.state() == sci_event_state::degraded)
        return sci_operation_result::resource_revoked;
    if (sci_event_.state() != sci_event_state::masked)
        return sci_operation_result::not_ready;
    if (transition_.active(now_after_clear.now_ns))
        return sci_operation_result::conflict;
    if (!sci_status_ledger_.commit_clear(sequence, masks))
        return sci_operation_result::conflict;
    return sci_operation_result::success;
}

sci_operation_result fixed_hardware::ack_and_rearm_sci(task::process_t *caller, u64 sequence, u32 &state, u32 &flags)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    const auto owner = caller->pid;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto owner_status = sci_owner_result_locked(owner);
        if (owner_status != sci_operation_result::success)
            return owner_status;
        if (!sci_event_.claimed() || sci_event_.state() == sci_event_state::degraded)
            return sci_operation_result::resource_revoked;
    }
    u64 pm1_status = 0;
    u64 pm1_status_b = 0;
    u64 gpe0_status = 0;
    u64 gpe1_status = 0;
    if (!read_optional_register(inventory_.pm1_event, pm1_status) ||
        !read_optional_register(inventory_.pm1_event_b, pm1_status_b) ||
        !read_optional_register(inventory_.gpe0_status, gpe0_status) ||
        !read_optional_register(inventory_.gpe1_status, gpe1_status))
    {
        (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
        mark_global_lock_event_lost();
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            sci_event_.rearm_failed();
            sci_status_ledger_.invalidate();
        }
        wake_global_lock_waiters();
        notify_readiness();
        return sci_operation_result::io_error;
    }
    if (((pm1_status | pm1_status_b) & pm1_global_lock_status) != 0)
        wake_global_lock_waiters();
    const bool pending_after_drain = has_pending(pm1_status, pm1_status_b, gpe0_status, gpe1_status);
    const auto now = sample_transition_clock();
    bool enable = false;
    bool degraded = false;
    auto failure = sci_operation_result::success;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto owner_status = sci_owner_result_locked(owner);
        if (owner_status != sci_operation_result::success)
            return owner_status;
        if (!sci_event_.claimed() || sci_event_.state() == sci_event_state::degraded)
            return sci_operation_result::resource_revoked;
        if (transition_.active(now.now_ns))
        {
            sci_event_.degrade();
            sci_status_ledger_.invalidate();
            degraded = true;
            failure = sci_operation_result::conflict;
        }
        else
        {
            const auto result = sci_event_.acknowledge(sequence, pending_after_drain, state, flags);
            sci_status_ledger_.invalidate();
            degraded = result == sci_ack_result::stale_sequence || result == sci_ack_result::conflict ||
                       result == sci_ack_result::degraded;
            if (result == sci_ack_result::stale_sequence)
                failure = sci_operation_result::stale_generation;
            else if (result == sci_ack_result::conflict)
                failure = sci_operation_result::conflict;
            else if (result == sci_ack_result::degraded)
                failure = sci_operation_result::resource_revoked;
            enable = result == sci_ack_result::rearmed;
        }
    }
    if (degraded)
    {
        (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
        notify_readiness();
        return failure;
    }
    if (enable && !arch::APIC::io_enable(static_cast<u8>(inventory_.sci_gsi)))
    {
        (void)arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi));
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            sci_event_.rearm_failed();
        }
        notify_readiness();
        return sci_operation_result::io_error;
    }
    notify_readiness();
    return sci_operation_result::success;
}

sci_operation_result fixed_hardware::prepare_sleep(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                                   u32 sleep_type_a, u32 sleep_type_b, u64 &ticket)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto owner_status = transition_owner_result_locked(caller->pid);
    if (owner_status != sci_operation_result::success)
        return owner_status;
    if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
        return sci_operation_result::stale_generation;
    if (inventory_.pm1_control.address == 0 || inventory_.pm1_control.length < 2)
        return sci_operation_result::not_supported;
    if (sleep_type_a > 7 || sleep_type_b > 7)
        return sci_operation_result::invalid_argument;
    if (!sci_event_.fixed_transition_ready())
        return sci_operation_result::would_block;
    const auto now = sample_transition_clock();
    switch (transition_.prepare(transition_kind::sleep, sleep_type_a, sleep_type_b, now.now_ns, now.cross_cpu_monotonic,
                                ticket))
    {
        case transition_prepare_result::prepared:
            return sci_operation_result::success;
        case transition_prepare_result::conflict:
            return sci_operation_result::conflict;
        case transition_prepare_result::exhausted:
            return sci_operation_result::resource_exhausted;
    }
    return sci_operation_result::resource_exhausted;
}

sci_operation_result fixed_hardware::commit_sleep(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                                  u64 ticket, u32 &outcome)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    u32 sleep_type_a = 0;
    u32 sleep_type_b = 0;
    const auto now = sample_transition_clock();
    bool disable_sci = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto owner_status = transition_owner_result_locked(caller->pid);
        if (owner_status != sci_operation_result::success)
            return owner_status;
        if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
            return sci_operation_result::stale_generation;
        if (!transition_.begin_commit(transition_kind::sleep, ticket, now.now_ns, sleep_type_a, sleep_type_b))
            return sci_operation_result::conflict;
        if (sci_event_.claimed())
        {
            sci_event_.mask_for_transition();
            disable_sci = true;
        }
    }

    if (disable_sci && !arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi)))
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            sci_event_.rearm_failed();
            transition_.finish();
        }
        outcome = 1;
        notify_readiness();
        return sci_operation_result::success;
    }

    const u64 control_mask = 0x1c00 | (1u << 13);
    bool written = false;
    bool write_attempted = false;
    written = with_pm1_control_update_lock<uctx::LockGuard_t<lock::mutex_t>>(pm1_control_update_lock_, [&] {
        u64 current_a = 0;
        u64 current_b = 0;
        if (!read_register(inventory_.pm1_control, current_a))
            return false;
        if (inventory_.pm1_control_b.address != 0 && !read_register(inventory_.pm1_control_b, current_b))
            return false;
        const u64 control_a = (current_a & ~control_mask) | (sleep_type_a << 10) | (1u << 13);
        write_attempted = true;
        bool update_written = write_register(inventory_.pm1_control, control_a);
        if (inventory_.pm1_control_b.address != 0)
        {
            const u64 control_b = (current_b & ~control_mask) | (sleep_type_b << 10) | (1u << 13);
            update_written = write_register(inventory_.pm1_control_b, control_b) && update_written;
        }
        return update_written;
    });
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (sci_event_.claimed())
            sci_event_.degrade();
        transition_.finish();
    }
    outcome = written ? 0u : write_attempted ? 2u : 1u;
    return sci_operation_result::success;
}

sci_operation_result fixed_hardware::prepare_reset(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                                   u64 &ticket)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto owner_status = transition_owner_result_locked(caller->pid);
    if (owner_status != sci_operation_result::success)
        return owner_status;
    if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
        return sci_operation_result::stale_generation;
    if (inventory_.reset.address == 0 || inventory_.reset.length == 0)
        return sci_operation_result::not_supported;
    if (!sci_event_.fixed_transition_ready())
        return sci_operation_result::would_block;
    const auto now = sample_transition_clock();
    switch (transition_.prepare(transition_kind::reset, 0, 0, now.now_ns, now.cross_cpu_monotonic, ticket))
    {
        case transition_prepare_result::prepared:
            return sci_operation_result::success;
        case transition_prepare_result::conflict:
            return sci_operation_result::conflict;
        case transition_prepare_result::exhausted:
            return sci_operation_result::resource_exhausted;
    }
    return sci_operation_result::resource_exhausted;
}

sci_operation_result fixed_hardware::commit_reset(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                                  u64 ticket, u32 &outcome)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    u32 unused_sleep_type_a = 0;
    u32 unused_sleep_type_b = 0;
    const auto now = sample_transition_clock();
    bool disable_sci = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto owner_status = transition_owner_result_locked(caller->pid);
        if (owner_status != sci_operation_result::success)
            return owner_status;
        if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
            return sci_operation_result::stale_generation;
        if (!transition_.begin_commit(transition_kind::reset, ticket, now.now_ns, unused_sleep_type_a,
                                      unused_sleep_type_b))
            return sci_operation_result::conflict;
        if (sci_event_.claimed())
        {
            sci_event_.mask_for_transition();
            disable_sci = true;
        }
    }
    if (disable_sci && !arch::APIC::io_disable(static_cast<u8>(inventory_.sci_gsi)))
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            sci_event_.rearm_failed();
            transition_.finish();
        }
        outcome = 1;
        notify_readiness();
        return sci_operation_result::success;
    }
    const bool written = write_register(inventory_.reset, inventory_.reset_value);
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (sci_event_.claimed())
            sci_event_.degrade();
        transition_.finish();
    }
    outcome = written ? 0u : 2u;
    return sci_operation_result::success;
}

sci_operation_result fixed_hardware::abort_transition(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                                      u64 ticket)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto owner_status = transition_owner_result_locked(caller->pid);
    if (owner_status != sci_operation_result::success)
        return owner_status;
    if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
        return sci_operation_result::stale_generation;
    return transition_.cancel(ticket, sample_transition_clock().now_ns) ? sci_operation_result::success
                                                                        : sci_operation_result::conflict;
}

sci_operation_result fixed_hardware::set_gpe_mask(task::process_t *caller, u64 boot_epoch, u64 firmware_generation,
                                                  u32 bank, u64 mask, bool enable)
{
    uctx::LockGuard_t<lock::mutex_t> transaction_guard(transaction_lock_);
    if (!is_live_caller(caller))
        return sci_operation_result::access_denied;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto owner_status = sci_owner_result_locked(caller->pid);
    if (owner_status != sci_operation_result::success)
        return owner_status;
    if (boot_epoch != inventory_.boot_epoch || firmware_generation != inventory_.firmware_generation)
        return sci_operation_result::stale_generation;
    if (sci_event_.state() == sci_event_state::degraded)
        return sci_operation_result::resource_revoked;
    if (!sci_event_.claimed())
        return sci_operation_result::not_ready;
    if (transition_.active(sample_transition_clock().now_ns))
        return sci_operation_result::conflict;
    if (bank > 1 || mask == 0)
        return sci_operation_result::invalid_argument;
    const auto &register_info = bank == 0 ? inventory_.gpe0_enable : inventory_.gpe1_enable;
    if (register_info.address == 0 || register_info.length == 0 || register_info.length > sizeof(u64))
        return sci_operation_result::not_supported;
    if (register_info.length * 8 < 64 && (mask >> (register_info.length * 8)) != 0)
        return sci_operation_result::invalid_argument;
    u64 current = 0;
    if (!read_optional_register(register_info, current))
        return sci_operation_result::io_error;
    const u64 next = enable ? current | mask : current & ~mask;
    return write_register(register_info, next) ? sci_operation_result::success : sci_operation_result::io_error;
}

global_lock_operation_result fixed_hardware::acquire_global_lock(task::process_t *caller,
                                                                 task::thread_t *execution_thread, u64 timeout_us)
{
    if (!is_live_caller(caller))
        return global_lock_operation_result::access_denied;
    if (execution_thread == nullptr || execution_thread->process != caller)
        return global_lock_operation_result::access_denied;

    const u64 now = timer::get_high_resolution_time();
    const u64 deadline = timeout_us == ~u64{0} || timeout_us > ~u64{0} - now ? ~u64{0} : now + timeout_us;
    const u64 tid = execution_thread->tid;
    const bool facs_lock_required = inventory_.facs_global_lock_address != 0 && !inventory_.hardware_reduced;

    const auto wait_for_change = [&](u64 observed_sequence, u64 wait_deadline) {
        const na_status_t status = naos::ipc::wait_for_condition(
            global_lock_wait_queue_,
            [this, observed_sequence] {
                return global_lock_wake_sequence_.load(std::memory_order_acquire) != observed_sequence;
            },
            wait_deadline);
        if (status == NA_STATUS_OK)
            return global_lock_operation_result::success;
        if (status == NA_STATUS_WAIT_TIMED_OUT || status == NA_STATUS_WOULD_BLOCK)
            return global_lock_operation_result::timed_out;
        return global_lock_operation_result::io_error;
    };

    bool owns_local_lock = false;
    for (;;)
    {
        const u64 observed_sequence = global_lock_wake_sequence_.load(std::memory_order_acquire);
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            if (facs_lock_required && global_lock_event_lost_)
                return global_lock_operation_result::unsupported;
            switch (classify_global_lock_owner_claim(global_lock_owner_pid_, global_lock_owner_tid_,
                                                     global_lock_acquire_pending_, global_lock_hardware_owned_,
                                                     caller->pid, tid))
            {
                case global_lock_owner_claim_decision::claim:
                    global_lock_owner_pid_ = caller->pid;
                    global_lock_owner_tid_ = tid;
                    global_lock_acquire_pending_ = true;
                    global_lock_hardware_owned_ = false;
                    owns_local_lock = true;
                    break;
                case global_lock_owner_claim_decision::already_owned:
                    return global_lock_operation_result::access_denied;
                case global_lock_owner_claim_decision::invalid_state:
                    return global_lock_operation_result::io_error;
                case global_lock_owner_claim_decision::contended:
                    break;
            }
        }
        if (owns_local_lock)
            break;
        if (timer::get_high_resolution_time() >= deadline)
            return global_lock_operation_result::timed_out;
        const auto wait_result = wait_for_change(observed_sequence, deadline);
        if (wait_result != global_lock_operation_result::success)
            return wait_result;
        if (!is_live_caller(caller))
            return global_lock_operation_result::access_denied;
    }

    const auto release_local_after_failed_acquire = [this, caller, tid] {
        bool released = false;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            if (global_lock_owner_pid_ == caller->pid && global_lock_owner_tid_ == tid && !global_lock_hardware_owned_)
            {
                global_lock_owner_pid_ = 0;
                global_lock_owner_tid_ = 0;
                global_lock_acquire_pending_ = false;
                released = true;
            }
        }
        if (released)
            wake_global_lock_waiters();
    };
    global_lock_arbitration_path arbitration_path = global_lock_arbitration_path::fail_closed;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (global_lock_owner_pid_ != caller->pid || global_lock_owner_tid_ != tid)
            return global_lock_operation_result::access_denied;
        arbitration_path =
            select_global_lock_arbitration_path(inventory_.facs_global_lock_address != 0, inventory_.hardware_reduced,
                                                global_lock_event_available_, global_lock_event_lost_);
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK)
        // q35 firmware always supplies a FACS. Force the no-FACS decision so
        // the local software serialization branch remains testable on stock QEMU.
        arbitration_path = global_lock_arbitration_path::software;
#endif
        if (arbitration_path == global_lock_arbitration_path::software)
            global_lock_acquire_pending_ = false;
    }
    if (arbitration_path == global_lock_arbitration_path::fail_closed)
    {
        release_local_after_failed_acquire();
        return global_lock_operation_result::unsupported;
    }
    if (arbitration_path == global_lock_arbitration_path::software)
    {
        // The PID/TID owner slot and wait queue remain held for the full
        // critical section; only the FACS compare-exchange is omitted.
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK)
        KLOG_INFO("ACPI TEST: forcing local Global Lock software serialization");
#endif
        return global_lock_operation_result::success;
    }

    u32 current = 0;
    if (!read_facs_global_lock(current))
    {
        release_local_after_failed_acquire();
        return global_lock_operation_result::io_error;
    }
    for (;;)
    {
        if (!is_live_caller(caller))
        {
            release_local_after_failed_acquire();
            return global_lock_operation_result::access_denied;
        }
        const u64 observed_sequence = global_lock_wake_sequence_.load(std::memory_order_acquire);
        const auto update = next_global_lock_acquire(current);
        u32 expected = current;
        if (!compare_exchange_facs_global_lock(expected, update.value))
        {
            current = expected;
            continue;
        }
        if (update.acquired)
        {
            bool still_owner = false;
            {
                uctx::RawSpinLockUninterruptibleContext guard(lock_);
                still_owner = global_lock_owner_pid_ == caller->pid && global_lock_owner_tid_ == tid;
                if (still_owner)
                {
                    global_lock_acquire_pending_ = false;
                    global_lock_hardware_owned_ = true;
                }
            }
            if (still_owner)
                return global_lock_operation_result::success;

            bool pending = false;
            const bool released = release_facs_global_lock(pending);
            const bool notified = released && (!pending || signal_global_lock_release());
            if (!notified)
            {
                {
                    uctx::RawSpinLockUninterruptibleContext guard(lock_);
                    global_lock_event_available_ = false;
                    global_lock_event_lost_ = true;
                }
                KLOG_ERROR("ACPI: failed to unwind FACS Global Lock after execution-thread exit");
            }
            wake_global_lock_waiters();
            return global_lock_operation_result::access_denied;
        }

#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE) && !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT) &&                     \
    !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
        const bool test_firmware_active = test_firmware_global_lock_active_.load(std::memory_order_acquire);
        const bool test_release_already_scheduled =
            test_firmware_active && test_firmware_release_scheduled_.exchange(true, std::memory_order_acq_rel);
        if (test_firmware_active && !test_release_already_scheduled)
        {
            const auto watcher = timer::schedule_after(
                100'000, timer::timer_handler::bind<&fixed_hardware::test_firmware_release_global_lock>(*this));
            if (watcher == timer::invalid_watcher_id)
            {
                mark_global_lock_event_lost();
                release_local_after_failed_acquire();
                return global_lock_operation_result::io_error;
            }
        }
#endif

        const bool poll_facs = arbitration_path == global_lock_arbitration_path::facs_poll;
        const u64 before_wait = timer::get_high_resolution_time();
        if (before_wait >= deadline)
        {
            release_local_after_failed_acquire();
            return global_lock_operation_result::timed_out;
        }
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE) && !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT) &&                     \
    !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
        test_global_lock_waiter_blocked_.store(true, std::memory_order_release);
#endif
        constexpr u64 facs_poll_interval_us = 1'000;
        const u64 poll_deadline = poll_facs && before_wait <= ~u64{0} - facs_poll_interval_us
                                      ? before_wait + facs_poll_interval_us
                                      : deadline;
        const u64 wait_deadline = poll_facs && poll_deadline < deadline ? poll_deadline : deadline;
        const auto wait_result = wait_for_change(observed_sequence, wait_deadline);
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE) && !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT) &&                     \
    !defined(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
        test_global_lock_waiter_blocked_.store(false, std::memory_order_release);
#endif
        if (wait_result != global_lock_operation_result::success)
        {
            if (poll_facs && wait_result == global_lock_operation_result::timed_out)
            {
                if (timer::get_high_resolution_time() >= deadline)
                {
                    release_local_after_failed_acquire();
                    return global_lock_operation_result::timed_out;
                }
                bool event_path_lost = false;
                {
                    uctx::RawSpinLockUninterruptibleContext guard(lock_);
                    event_path_lost = global_lock_event_lost_;
                }
                if (event_path_lost)
                {
                    release_local_after_failed_acquire();
                    return global_lock_operation_result::unsupported;
                }
                if (!read_facs_global_lock(current))
                {
                    release_local_after_failed_acquire();
                    return global_lock_operation_result::io_error;
                }
                continue;
            }
            release_local_after_failed_acquire();
            return wait_result;
        }
        if (timer::get_high_resolution_time() >= deadline)
        {
            release_local_after_failed_acquire();
            return global_lock_operation_result::timed_out;
        }
        bool global_lock_event_lost = false;
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            global_lock_event_lost = global_lock_event_lost_;
        }
        if (global_lock_event_lost)
        {
            release_local_after_failed_acquire();
            return global_lock_operation_result::unsupported;
        }
        if (!read_facs_global_lock(current))
        {
            release_local_after_failed_acquire();
            return global_lock_operation_result::io_error;
        }
    }
}

global_lock_operation_result fixed_hardware::release_global_lock(task::process_t *caller,
                                                                 task::thread_t *execution_thread)
{
    if (!is_live_caller(caller))
        return global_lock_operation_result::access_denied;
    if (execution_thread == nullptr || execution_thread->process != caller)
        return global_lock_operation_result::access_denied;
    const u64 tid = execution_thread->tid;
    bool hardware_owned = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (global_lock_owner_pid_ != caller->pid || global_lock_owner_tid_ != tid)
            return global_lock_operation_result::not_acquired;
        if (global_lock_acquire_pending_)
            return global_lock_operation_result::not_acquired;
        hardware_owned = global_lock_hardware_owned_;
    }

    bool pending = false;
    bool release_failed = false;
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
    const bool release_failure_injected =
        hardware_owned && test_global_lock_release_failure_pending_.exchange(false, std::memory_order_acq_rel);
    release_failed = release_failure_injected;
#endif
    if (hardware_owned && !release_failed)
        release_failed = !release_facs_global_lock(pending);
    if (release_failed)
    {
        {
            uctx::RawSpinLockUninterruptibleContext guard(lock_);
            global_lock_event_lost_ = true;
            global_lock_event_available_ = false;
            if (global_lock_owner_pid_ == caller->pid && global_lock_owner_tid_ == tid)
            {
                global_lock_owner_pid_ = 0;
                global_lock_owner_tid_ = 0;
                global_lock_acquire_pending_ = false;
                global_lock_hardware_owned_ = false;
            }
        }
        wake_global_lock_waiters();
#if defined(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
        if (release_failure_injected)
            KLOG_INFO("ACPI TEST: FACS Global Lock release-failure path quarantined");
#endif
        return global_lock_operation_result::io_error;
    }
    const bool notification_succeeded = !pending || signal_global_lock_release();
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (global_lock_owner_pid_ == caller->pid && global_lock_owner_tid_ == tid)
        {
            global_lock_owner_pid_ = 0;
            global_lock_owner_tid_ = 0;
            global_lock_acquire_pending_ = false;
            global_lock_hardware_owned_ = false;
        }
        if (!notification_succeeded)
        {
            global_lock_event_lost_ = true;
            global_lock_event_available_ = false;
        }
    }
    wake_global_lock_waiters();
    return notification_succeeded ? global_lock_operation_result::success : global_lock_operation_result::io_error;
}

na_signal_t fixed_hardware::capability_signals() const
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    return sci_event_.readable() ? NA_SIGNAL_READABLE : 0;
}

u64 fixed_hardware::capability_state() const
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    return sci_event_.sequence();
}

u32 fixed_hardware::sci_state_value() const
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    return sci_event_.state_value();
}

bool fixed_hardware::read_register(const fixed_register &register_info, u64 &value)
{
    const u8 access_width = register_info.access_width;
    if (register_info.address == 0 || register_info.length == 0 || register_info.length > sizeof(u64) ||
        (access_width != 1 && access_width != 2 && access_width != 4 && access_width != 8) ||
        register_info.length % access_width != 0 || (register_info.space == 1 && access_width > sizeof(u32)))
        return false;
    value = 0;
    for (u32 offset = 0; offset < register_info.length; offset += access_width)
    {
        u64 scalar = 0;
        if (!read_register_scalar(register_info, offset, access_width, scalar))
            return false;
        value |= scalar << (offset * 8);
    }
    return true;
}

bool fixed_hardware::read_optional_register(const fixed_register &register_info, u64 &value)
{
    if (register_info.address == 0 && register_info.length == 0)
    {
        value = 0;
        return true;
    }
    return read_register(register_info, value);
}

bool fixed_hardware::write_register(const fixed_register &register_info, u64 value)
{
    const u8 access_width = register_info.access_width;
    if (register_info.address == 0 || register_info.length == 0 || register_info.length > sizeof(u64) ||
        (access_width != 1 && access_width != 2 && access_width != 4 && access_width != 8) ||
        register_info.length % access_width != 0 || (register_info.space == 1 && access_width > sizeof(u32)))
        return false;
    for (u32 offset = 0; offset < register_info.length; offset += access_width)
    {
        if (!write_register_scalar(register_info, offset, access_width, value >> (offset * 8)))
            return false;
    }
    return true;
}

} // namespace acpi
