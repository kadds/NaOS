#include "kernel/smp.hpp"
#include "kernel/arch/smp.hpp"
#include "kernel/cpu.hpp"
#include "kernel/lock.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
namespace SMP
{
void init() { arch::SMP::init(); }

std::atomic_int wait_counter;

void wait_sync()
{
    int cpu_count = cpu::count() - 1;
    int exp = 0;
    if (!wait_counter.compare_exchange_strong(exp, cpu_count))
    {
        wait_counter--;
    }

    while (wait_counter != 0)
    {
        cpu_pause();
    }

    wait_counter = 0;
}
} // namespace SMP
