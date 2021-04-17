#pragma once
#include "common.hpp"

namespace arch::cpu_info
{
enum class feature
{
    system_call_ret,
    pcid,
    fpu,
    huge_page_1gb,
    apic,
    xapic,
    x2apic,
    msr,
    tsc,
    contant_tsc,
    sse,
    sse2,
    sse3,
    ssse3,
    sse4_1,
    sse4_2,
    popcnt_i,
    max_phy_addr,
    max_virt_addr,
    htt
};

void init();
/// check if has the feature
bool has_feature(feature f);

/// get the feature value
u64 get_feature(feature f);

const char *get_cpu_manufacturer();

} // namespace arch::cpu_info