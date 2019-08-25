#pragma once
#include "common.hpp"
namespace cpu
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
void trace_debug_info();

bool has_future(future f);
u64 get_future(future f);
const char *get_cpu_manufacturer();
} // namespace cpu
