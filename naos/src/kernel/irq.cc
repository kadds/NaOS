#include "kernel/irq.hpp"
#include "freelibcxx/linked_list.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/idt.hpp"
#include "kernel/arch/interrupt.hpp"
#include "kernel/cpu.hpp"
#include "kernel/irq/soft_irq_drain.hpp"
#include "kernel/irq/vector_pool.hpp"
#include "kernel/lock.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/tasklet.hpp"
#include "kernel/types.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/wait.hpp"
#include <atomic>
KLOG_MODULE(kernel);
namespace irq
{
namespace
{
vector_pool interrupt_vector_pool;
lock::spinlock_t interrupt_vector_pool_lock;

bool release_msi_vector(u8 vector) noexcept
{
    uctx::RawSpinLockUninterruptibleContext guard(interrupt_vector_pool_lock);
    return interrupt_vector_pool.release_msi_vector(vector);
}

bool quarantine_msi_vector(u8 vector) noexcept
{
    uctx::RawSpinLockUninterruptibleContext guard(interrupt_vector_pool_lock);
    return interrupt_vector_pool.quarantine_msi_vector(vector);
}
} // namespace

struct request_entry
{
    u64 id;
    hard_handler handler;

    request_entry(u64 id, hard_handler handler)
        : id(id)
        , handler(handler)
    {
    }

    request_result invoke(const interrupt_info *info, u64 extra_data) const noexcept
    {
        return handler(info, extra_data);
    }
};

struct soft_request_entry
{
    u64 id;
    soft_handler handler;

    soft_request_entry(u64 id, soft_handler handler)
        : id(id)
        , handler(handler)
    {
    }

    void invoke(u64 vector) const noexcept { handler(vector); }
};

using request_list_t = freelibcxx::linked_list<request_entry>;
using soft_request_list_t = freelibcxx::linked_list<soft_request_entry>;
constexpr int irq_count = 256;

template <typename Entry> struct request_lock_list_t
{
    lock::rw_lock_t lock;
    freelibcxx::linked_list<Entry> *list;
    explicit request_lock_list_t()
        : list(memory::New<freelibcxx::linked_list<Entry>>(memory::KernelCommonAllocatorV,
                                                           memory::KernelCommonAllocatorV))
    {
    }
};

template <typename Entry> struct irq_info_t
{
    request_lock_list_t<Entry> list;
    std::atomic_uint64_t counter;
};

irq_info_t<request_entry> *irq_info_list;

irq_info_t<soft_request_entry> *soft_irq_info_list;

std::atomic_uint64_t next_registration_id{1};

static request_result absorb_quarantined_msi(const interrupt_info *, u64) noexcept { return request_result::ok; }

static bool quarantine_hard_registration(u32 vector, u64 id) noexcept
{
    if (vector >= irq_count || id == 0 || irq_info_list == nullptr)
        return false;
    auto &locked_list = irq_info_list[vector].list;
    uctx::RawWriteLockUninterruptibleContext guard(locked_list.lock);
    for (auto &entry : *locked_list.list)
    {
        if (entry.id != id)
            continue;
        entry.handler = hard_handler::bind<&absorb_quarantined_msi>();
        return true;
    }
    return false;
}

constexpr u32 max_soft_irq_drain_passes = 8;

bool _ctx_interrupt_ do_irq(const regs_t *regs, u64 extra_data)
{
    interrupt_info inter;
    inter.kernel_space = (regs->cs & 0x3) == 0;
    inter.regs = (void *)regs;
    inter.at = (void *)regs->rip;
    inter.error_code = regs->error_code;
    auto &locked_list = irq_info_list[regs->vector].list;
    auto &info = irq_info_list[regs->vector];
    info.counter++;
    // if (regs->vector != 14 && regs->vector != 34)
    // {
    //     if ((regs->vector == 128 && info.counter % 200 == 0) || regs->vector != 128)
    //     {
    //         KLOG_INFO("inter vector {} times {}", regs->vector, info.counter.load());
    //     }
    // }

    // The read lock is deliberately the callback quiescence barrier: a
    // registration reset takes the write lock and therefore cannot return
    // until this interrupt has left the handler.  Keep interrupts disabled
    // while holding it; otherwise a nested handler on this CPU can recurse
    // into registration teardown and wait forever on its own read lock.
    uctx::RawReadLockUninterruptibleContext icu(locked_list.lock);
    bool ok = false;
    for (auto &it : *locked_list.list)
    {
        auto ret = it.invoke(&inter, extra_data);
        if (ret == request_result::ok)
            ok = true;
    }
    return ok;
}

bool wakeup_condition();

void do_soft_irq()
{
    auto &cpu = arch::cpu::current();
    detail::drain_requeued_soft_interrupts(
        static_cast<u32>(soft_vector::COUNT), max_soft_irq_drain_passes,
        [&cpu](u32 vector) {
            auto &locked_list = soft_irq_info_list[vector].list;
            uctx::RawReadLockUninterruptibleController ctr(locked_list.lock);
            ctr.begin();
            bool consumed = false;
            if (cpu.is_irq_pending(vector))
            {
                cpu.clean_irq_pending(vector);
                soft_request_list_t &list = *locked_list.list;
                for (auto &entry : list)
                    entry.invoke(vector);
                consumed = true;
            }
            ctr.end();
            return consumed;
        },
        [] { return wakeup_condition(); });
    if (unlikely(wakeup_condition()))
        cpu::current().get_soft_irq_wait_queue()->do_wake_up();
}

bool check_and_wakeup_soft_irq(const regs_t *regs, u64 extra_data)
{
    (void)regs;
    (void)extra_data;
    do_soft_irq();
    return true;
}

bool wakeup_condition()
{
    auto &cpu = arch::cpu::current();
    for (int i = 0; i < soft_vector::COUNT; i++)
    {
        if (cpu.is_irq_pending(i))
            return true;
    }
    return false;
}

void wakeup_soft_irq_daemon()
{
    cpu::current().get_soft_irq_wait_queue()->do_wait(wakeup_condition);
    do_soft_irq();
}

void raise_soft_irq(u64 soft_irq_number)
{
    uctx::UninterruptibleContext uic;
    arch::cpu::current().set_irq_pending(soft_irq_number);
}

void init()
{
    if (cpu::current().is_bsp())
    {
        irq_info_list = memory::NewArray<irq_info_t<request_entry>>(memory::KernelBuddyAllocatorV, irq_count);
        soft_irq_info_list =
            memory::NewArray<irq_info_t<soft_request_entry>>(memory::KernelCommonAllocatorV, soft_vector::COUNT);
    }
    arch::exception::set_callback(&do_irq);
    arch::interrupt::set_callback(&do_irq);
    arch::interrupt::set_soft_irq_callback(&check_and_wakeup_soft_irq);
    init_tasklet();
    arch::idt::enable();
}

void unregister_hard_handler(u32 vector, u64 id)
{
    auto &locked_list = irq_info_list[vector].list;
    uctx::RawWriteLockUninterruptibleContext icu(locked_list.lock);
    auto &list = *locked_list.list;
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (it->id == id)
        {
            list.remove(it);
            return;
        }
    }
}

void unregister_soft_handler(u32 vector, u64 id)
{
    auto &locked_list = soft_irq_info_list[vector].list;
    uctx::RawWriteLockUninterruptibleContext icu(locked_list.lock);
    auto &list = *locked_list.list;
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (it->id == id)
        {
            list.remove(it);
            return;
        }
    }
}

registration::registration(registration &&other) noexcept
    : kind_(other.kind_)
    , vector_(other.vector_)
    , id_(other.id_)
{
    other.kind_ = kind::none;
    other.vector_ = 0;
    other.id_ = 0;
}

registration &registration::operator=(registration &&other) noexcept
{
    if (this != &other)
    {
        reset();
        kind_ = other.kind_;
        vector_ = other.vector_;
        id_ = other.id_;
        other.kind_ = kind::none;
        other.vector_ = 0;
        other.id_ = 0;
    }
    return *this;
}

registration::~registration() { reset(); }

void registration::reset() noexcept
{
    const auto kind = kind_;
    const auto vector = vector_;
    const auto id = id_;
    kind_ = kind::none;
    vector_ = 0;
    id_ = 0;
    if (id == 0)
        return;
    if (kind == kind::hard)
        unregister_hard_handler(vector, id);
    else if (kind == kind::soft)
        unregister_soft_handler(vector, id);
}

bool registration::quarantine() noexcept
{
    if (kind_ != kind::hard || id_ == 0 || !quarantine_hard_registration(vector_, id_))
        return false;
    kind_ = kind::none;
    vector_ = 0;
    id_ = 0;
    return true;
}

msi_vector_lease::msi_vector_lease(msi_vector_lease &&other) noexcept
    : vector_(other.vector_)
{
    other.vector_ = 0;
}

msi_vector_lease &msi_vector_lease::operator=(msi_vector_lease &&other) noexcept
{
    if (this != &other)
    {
        reset();
        vector_ = other.vector_;
        other.vector_ = 0;
    }
    return *this;
}

msi_vector_lease::~msi_vector_lease() { reset(); }

void msi_vector_lease::reset() noexcept
{
    const u8 vector = vector_;
    vector_ = 0;
    if (vector != 0)
        (void)release_msi_vector(vector);
}

bool msi_vector_lease::quarantine() noexcept
{
    if (vector_ == 0 || !quarantine_msi_vector(vector_))
        return false;
    vector_ = 0;
    return true;
}

u8 allocate_ioapic_vector() noexcept
{
    uctx::RawSpinLockUninterruptibleContext guard(interrupt_vector_pool_lock);
    return interrupt_vector_pool.allocate_ioapic_vector();
}

bool release_ioapic_vector(u8 vector) noexcept
{
    uctx::RawSpinLockUninterruptibleContext guard(interrupt_vector_pool_lock);
    return interrupt_vector_pool.release_ioapic_vector(vector);
}

msi_vector_lease allocate_msi_vector() noexcept
{
    uctx::RawSpinLockUninterruptibleContext guard(interrupt_vector_pool_lock);
    return msi_vector_lease(interrupt_vector_pool.allocate_msi_vector());
}

registration register_handler(u32 vector, hard_handler handler)
{
    if (!handler || vector >= irq_count)
        return {};
    const auto id = next_registration_id.fetch_add(1);
    auto &locked_list = irq_info_list[vector].list;
    uctx::RawWriteLockUninterruptibleContext icu(locked_list.lock);
    locked_list.list->push_back(request_entry(id, handler));
    return registration(registration::kind::hard, vector, id);
}

registration register_soft_handler(u32 vector, soft_handler handler)
{
    if (!handler)
        return {};
    const auto id = next_registration_id.fetch_add(1);
    auto &locked_list = soft_irq_info_list[vector].list;
    uctx::RawWriteLockUninterruptibleContext icu(locked_list.lock);
    locked_list.list->push_back(soft_request_entry(id, handler));
    return registration(registration::kind::soft, vector, id);
}

} // namespace irq
