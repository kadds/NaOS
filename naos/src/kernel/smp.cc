#include "kernel/smp.hpp"
#include "kernel/arch/smp.hpp"
#include "kernel/cpu.hpp"
#include "kernel/lock.hpp"
#include "kernel/trace.hpp"
#include "kernel/ucontext.hpp"
namespace SMP
{
std::atomic_int wait_counter = 0;

void init() { arch::SMP::init(); }

} // namespace SMP
