#include "kernel/schedulers/completely_fair.hpp"
#include "kernel/schedulers/round_robin.hpp"
#include "kernel/terminal_identity.hpp"
#include "kernel/timer.hpp"

#include "catch2_compat.hpp"
#include <cstdlib>
#include <stdexcept>
#include <string>

// Run the production scheduler backends with a host CPU and allocator. No
// scheduler transitions or queue operations are replaced by the fixture.
namespace
{
cpu::cpu_data_t *host_cpu = nullptr;
u32 host_cpu_index = 0;
bool host_interrupts_enabled = true;
std::string panic_message;
u64 clock_us = 0;
memory::KernelCommonAllocator host_allocator;
} // namespace

namespace memory
{
KernelCommonAllocator *KernelCommonAllocatorV = &host_allocator;
void *KernelCommonAllocator::allocate(u64 size, u64) noexcept { return std::malloc(size); }
void KernelCommonAllocator::deallocate(void *pointer) noexcept { std::free(pointer); }
} // namespace memory

namespace arch::idt
{
void enable() { host_interrupts_enabled = true; }
void disable() { host_interrupts_enabled = false; }
bool save_and_disable()
{
    const bool previous = host_interrupts_enabled;
    disable();
    return previous;
}
bool is_enable() { return host_interrupts_enabled; }
} // namespace arch::idt

namespace cpu
{
cpu_data_t &current() { return *host_cpu; }
u32 cpu_data_t::id() { return host_cpu_index; }
void cpu_data_t::set_task(task::thread_t *thread) { current_task.store(thread); }
} // namespace cpu

namespace timer
{
timeclock::microsecond_t get_high_resolution_time() { return ++clock_us; }
} // namespace timer

namespace log
{
bool enabled(level, module) { return false; }
namespace detail
{
void emit_message(level, module, const char *, u32, bool, const char *, u64) {}
void emit_panic_message(module, const char *, u32, bool, const char *message, u64 length, const regs_t *)
{
    panic_message.assign(message, length);
}
} // namespace detail
NoReturn void keep_panic(const regs_t *) { throw std::runtime_error(panic_message); }
} // namespace log

namespace freelibcxx
{
void assert_fail(const char *expression, const char *, int, const char *) { throw std::runtime_error(expression); }
} // namespace freelibcxx

void get_task_id(u64 &pid, u64 &tid)
{
    auto *thread = host_cpu == nullptr ? nullptr : host_cpu->get_task();
    pid = thread == nullptr || thread->process == nullptr ? 0 : thread->process->pid;
    tid = thread == nullptr ? 0 : thread->tid;
}

namespace task
{
// These objects supply identity and storage only; process creation, capability
// operations and physical context switching are outside the backend tests.
resource_table_t::resource_table_t()
    : native_handle_map(memory::KernelCommonAllocatorV)
    , next_native_handle(1)
    , native_entry_count(0)
{
}
resource_table_t::~resource_table_t() = default;
process_t::process_t()
    : command_line(memory::KernelCommonAllocatorV)
    , attributes(0)
{
}
thread_t::thread_t()
    : state(thread_state::destroy)
    , attributes(0)
    , tid(0)
    , process(nullptr)
    , scheduler(nullptr)
    , schedule_data(nullptr)
    , register_info(nullptr)
    , kernel_stack_top(nullptr)
    , user_stack_top(nullptr)
    , user_stack_bottom(nullptr)
    , wait_counter(0)
{
}
void switch_thread(thread_t *, thread_t *next)
{
    REQUIRE_FALSE(arch::idt::is_enable());
    cpu::current().set_task(next);
}
} // namespace task

namespace
{
using namespace task;

struct scheduler_fixture
{
    cpu::cpu_data_t cpu;
    process_t idle_process;
    process_t process;
    thread_t idle;
    thread_t first;
    thread_t second;
    scheduler::completely_fair_scheduler cfs;
    scheduler::round_robin_scheduler rr;
    scheduler::scheduler &backend;
    bool first_owned = true;
    bool second_owned = true;

    explicit scheduler_fixture(bool realtime)
        : backend(realtime ? static_cast<scheduler::scheduler &>(rr) : static_cast<scheduler::scheduler &>(cfs))
    {
        host_cpu = &cpu;
        host_cpu_index = 0;
        host_interrupts_enabled = true;
        idle_process.pid = 0;
        process.pid = 42;
        process.mm_info = &process;
        idle.process = &idle_process;
        idle.state = thread_state::running;
        cpu.set_task(&idle);
        cpu.set_idle_task(&idle);
        cfs.init_cpu();
        rr.init_cpu();
        first.process = second.process = &process;
        first.scheduler = second.scheduler = &backend;
        first.state = thread_state::ready;
        second.state = thread_state::ready;
        first.tid = 1;
        second.tid = 2;
        backend.add(&first);
        backend.add(&second);
        idle.attributes |= thread_attributes::need_schedule;
        backend.schedule();
        REQUIRE(cpu.get_task() == &first);
        REQUIRE(backend.scheduleable_task_count() == 1);
    }

    ~scheduler_fixture()
    {
        if (first_owned)
            backend.remove(&first);
        if (second_owned)
            backend.remove(&second);
        memory::KernelCommonAllocatorV->deallocate(idle.schedule_data);
        rr.destroy_cpu();
        cfs.destroy_cpu();
        host_cpu = nullptr;
    }

    void remove_second()
    {
        second_owned = false;
        backend.remove(&second);
    }
};
} // namespace

TEST_CASE("RR expires a time slice without duplicating the outgoing runnable", "[scheduler]")
{
    scheduler_fixture fixture(true);
    for (int tick = 0; tick < 100 && !(fixture.first.attributes & task::thread_attributes::need_schedule); ++tick)
        fixture.backend.schedule_tick();
    REQUIRE(fixture.first.attributes & task::thread_attributes::need_schedule);
    fixture.backend.schedule();
    REQUIRE(fixture.cpu.get_task() == &fixture.second);
    REQUIRE(fixture.backend.scheduleable_task_count() == 1);
}

TEST_CASE("scheduler block requests are idempotent", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        fixture.backend.update_state(&fixture.second, task::thread_state::stop);
        REQUIRE_NOTHROW(fixture.backend.update_state(&fixture.second, task::thread_state::stop));
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
    }
}

TEST_CASE("scheduler removal releases and clears metadata exactly once", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        fixture.remove_second();
        REQUIRE(fixture.second.schedule_data == nullptr);
        REQUIRE_NOTHROW(fixture.backend.remove(&fixture.second));
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
    }
}

TEST_CASE("a wake before switch out cancels the pending block", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        fixture.backend.update_state(&fixture.first, task::thread_state::stop);
        REQUIRE(fixture.first.state == task::thread_state::running);
        REQUIRE(fixture.first.attributes & task::thread_attributes::block_to_stop);
        fixture.backend.update_state(&fixture.first, task::thread_state::ready);
        REQUIRE(fixture.first.state == task::thread_state::running);
        REQUIRE_FALSE(fixture.first.attributes & task::thread_attributes::block_to_stop);
        REQUIRE(fixture.backend.scheduleable_task_count() == 1);
    }
}

TEST_CASE("ordinary wakes preserve job control stops", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        fixture.backend.update_state(&fixture.second, task::thread_state::stop);
        fixture.second.attributes |= task::thread_attributes::job_control_stopped;
        fixture.backend.update_state(&fixture.second, task::thread_state::ready);
        REQUIRE(fixture.second.state == task::thread_state::stop);
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
        fixture.second.attributes &= ~task::thread_attributes::job_control_stopped;
        fixture.backend.update_state(&fixture.second, task::thread_state::ready);
        REQUIRE(fixture.second.state == task::thread_state::ready);
        REQUIRE(fixture.backend.scheduleable_task_count() == 1);
    }
}

TEST_CASE("late wakes cannot requeue an exiting thread", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        fixture.backend.update_state(&fixture.second, task::thread_state::stop);
        REQUIRE(fixture.second.exit_lifecycle.try_begin_exit());
        fixture.backend.update_state(&fixture.second, task::thread_state::ready);
        REQUIRE(fixture.second.state == task::thread_state::stop);
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
    }
}

TEST_CASE("exiting threads leave the CPU without returning to a scheduler queue", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        REQUIRE(fixture.first.exit_lifecycle.try_begin_exit());
        fixture.first.attributes |= task::thread_attributes::need_schedule;
        fixture.backend.schedule();
        REQUIRE(fixture.first.state == task::thread_state::stop);
        REQUIRE(fixture.cpu.get_task() == &fixture.second);
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
    }
}

TEST_CASE("dispatch allows cooperative process exit", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        fixture.process.attributes |= task::process_attributes::exiting;
        fixture.backend.update_state(&fixture.first, task::thread_state::stop);
        fixture.backend.schedule();
        REQUIRE(fixture.cpu.get_task() == &fixture.second);
    }
}

TEST_CASE("dispatch skips threads with an exit claim", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        REQUIRE(fixture.second.exit_lifecycle.try_begin_exit());
        fixture.backend.update_state(&fixture.first, task::thread_state::stop);
        if (!fixture.backend.schedule())
            fixture.cfs.schedule();
        REQUIRE(fixture.cpu.get_task() == &fixture.idle);
    }
}

TEST_CASE("a late wake cannot cancel a running thread's exit", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        REQUIRE(fixture.first.exit_lifecycle.try_begin_exit());
        fixture.backend.update_state(&fixture.first, task::thread_state::stop);
        fixture.backend.update_state(&fixture.first, task::thread_state::ready);
        REQUIRE(fixture.first.attributes & task::thread_attributes::block_to_stop);
        REQUIRE(fixture.first.attributes & task::thread_attributes::need_schedule);
        fixture.backend.schedule();
        REQUIRE(fixture.first.state == task::thread_state::stop);
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
    }
}

TEST_CASE("switch out preparation is idempotent", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        fixture.backend.prepare_switch_out(&fixture.first);
        fixture.backend.prepare_switch_out(&fixture.first);
        REQUIRE(fixture.backend.scheduleable_task_count() == 2);
        fixture.backend.update_state(&fixture.first, task::thread_state::stop);
        fixture.backend.prepare_switch_out(&fixture.first);
        fixture.backend.prepare_switch_out(&fixture.first);
        REQUIRE(fixture.backend.scheduleable_task_count() == 1);
        fixture.backend.update_state(&fixture.first, task::thread_state::ready);
        REQUIRE(fixture.backend.scheduleable_task_count() == 2);
    }
}

TEST_CASE("completed threads ignore late wakes and preserve status encoding", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        REQUIRE(fixture.second.exit_lifecycle.try_begin_exit());
        REQUIRE(fixture.second.status_attributes() & task::thread_attributes::exit_pending);
        REQUIRE_FALSE(fixture.second.attributes & task::thread_attributes::exit_pending);
        fixture.remove_second();
        fixture.second.state = task::thread_state::destroy;
        REQUIRE(fixture.second.exit_lifecycle.complete_exit());
        REQUIRE_FALSE(fixture.second.status_attributes() & task::thread_attributes::exit_pending);
        REQUIRE(static_cast<unsigned>(fixture.second.state) == 3);
        REQUIRE_NOTHROW(fixture.backend.update_state(&fixture.second, task::thread_state::ready));
        REQUIRE(fixture.second.state == task::thread_state::destroy);
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
    }
}

TEST_CASE("a process with released execution resources cannot be dispatched", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        for (auto flags : {task::process_attributes::no_thread, task::process_attributes::destroy,
                           task::process_attributes::userspace})
        {
            scheduler_fixture fixture(realtime);
            fixture.process.attributes = flags;
            fixture.process.mm_info = nullptr;
            fixture.backend.update_state(&fixture.first, task::thread_state::stop);
            if (!fixture.backend.schedule())
                fixture.cfs.schedule();
            REQUIRE(fixture.cpu.get_task() == &fixture.idle);
        }
    }
}

TEST_CASE("migration transfers queue membership and policy metadata", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        auto *source_data = fixture.second.schedule_data;
        fixture.second.attributes |= task::thread_attributes::on_migrate;
        REQUIRE(fixture.backend.commit_migrate(&fixture.second));
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
        if (realtime)
            REQUIRE(fixture.second.schedule_data == nullptr);
        else
            REQUIRE(fixture.second.schedule_data == source_data);

        cpu::cpu_data_t target_cpu;
        task::thread_t target_idle;
        target_idle.process = &fixture.idle_process;
        target_idle.state = task::thread_state::running;
        target_cpu.set_idle_task(&target_idle);
        target_cpu.set_task(&target_idle);
        host_cpu = &target_cpu;
        host_cpu_index = 1;
        fixture.cfs.init_cpu();
        fixture.rr.init_cpu();
        fixture.backend.on_migrate(&fixture.second);
        fixture.second.attributes &= ~task::thread_attributes::on_migrate;
        REQUIRE(fixture.second.cpuid == 1);
        REQUIRE(fixture.second.schedule_data != nullptr);
        REQUIRE(fixture.backend.scheduleable_task_count() == 1);
        target_idle.attributes |= task::thread_attributes::need_schedule;
        fixture.backend.schedule();
        REQUIRE(target_cpu.get_task() == &fixture.second);
        REQUIRE(fixture.backend.scheduleable_task_count() == 0);
        fixture.remove_second();
        memory::KernelCommonAllocatorV->deallocate(target_idle.schedule_data);
        fixture.rr.destroy_cpu();
        fixture.cfs.destroy_cpu();
        host_cpu = &fixture.cpu;
        host_cpu_index = 0;
    }
}

TEST_CASE("kernel process workers are distinct from the CPU idle thread", "[scheduler]")
{
    for (bool realtime : {false, true})
    {
        scheduler_fixture fixture(realtime);
        fixture.first.process = &fixture.idle_process;
        fixture.second.process = &fixture.idle_process;
        fixture.backend.update_state(&fixture.second, task::thread_state::stop);
        fixture.backend.update_state(&fixture.second, task::thread_state::ready);
        REQUIRE(fixture.backend.scheduleable_task_count() == 1);
        fixture.backend.prepare_switch_out(&fixture.first);
        REQUIRE(fixture.backend.scheduleable_task_count() == 2);
    }
}
