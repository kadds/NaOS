#include "kernel/smp.hpp"
#include "kernel/arch/smp.hpp"
namespace SMP
{
void init() { arch::SMP::init(); }

} // namespace SMP
