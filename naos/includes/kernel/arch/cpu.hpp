#pragma once
#include "common.hpp"

namespace arch::cpu
{
enum class future
{
    system_call_ret,
    pcid,
    fpu,
    huge_page_1gb,
    max_phy_addr,
    max_virt_addr,
};

void init();
// check if has the future
bool has_future(future f);

// get the future value
u64 get_future(future f);

const char *get_cpu_manufacturer();

} // namespace arch::cpu
