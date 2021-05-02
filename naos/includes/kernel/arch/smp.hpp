#pragma once
namespace arch::SMP
{
void init();
extern volatile void *ap_stack;
extern volatile void *ap_stack_phy;

} // namespace arch::SMP
