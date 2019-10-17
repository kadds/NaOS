#pragma once
#include "common.hpp"

namespace arch::cpu_info
{
enum class future
{
    system_call_ret,
    pcid,
    fpu,
    huge_page_1gb,
    apic,
    xapic,
    x2apic,
    msr,
    max_phy_addr,
    max_virt_addr
};

void init();
// check if has the future
bool has_future(future f);

// get the future value
u64 get_future(future f);

enum class tsc_type
{
    future,
    constant,
    nonstop,
};
tsc_type get_tsc_future();

const char *get_cpu_manufacturer();

} // namespace arch::cpu_info