#pragma once
#include "common.hpp"
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/mm/new.hpp"
#include <string>

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
    constant_tsc,
    nostop_tsc,
    sse,
    sse2,
    sse3,
    ssse3,
    sse4_1,
    sse4_2,
    popcnt_i,
    max_phy_addr,
    max_virt_addr,
    htt,
    xsave,
    osxsave,
    avx,

    crystal_frequency,
    tsc_frequency,
    cpu_base_frequency,
    cpu_max_frequency,
    bus_frequency,
};

void init();
/// check if has the feature
bool has_feature(feature f);

/// get the feature value
u64 get_feature(feature f);

struct logic_core_id
{
    u8 numa_index = 0;
    u8 chip_index = 0;
    u8 core_index = 0;
    u8 logic_index = 0;
    size_t hash() const
    {
        u64 ret = numa_index;
        ret <<= 8;
        ret |= chip_index;
        ret <<= 8;
        ret |= core_index;
        ret <<= 8;
        ret |= logic_index;
        return ret;
    }
    bool operator==(const logic_core_id &rhs) const
    {
        return numa_index == rhs.numa_index && chip_index == rhs.chip_index && core_index == rhs.core_index &&
               logic_index == rhs.logic_index;
    }
    bool operator!=(const logic_core_id &rhs) const { return !operator==(rhs); }
};

struct logic_core
{
    int apic_id;
    int apic_process_id;
    bool enabled = false;
    bool exist = false;
};

struct core
{
    core()
        : logics(memory::MemoryAllocatorV)
    {
    }
    freelibcxx::vector<logic_core> logics;
};

struct chip_node
{
    chip_node()
        : cores(memory::MemoryAllocatorV)
    {
    }
    freelibcxx::vector<core> cores;
};

struct numa_node
{
    numa_node()
        : chip(memory::MemoryAllocatorV)
    {
    }
    freelibcxx::vector<chip_node> chip;
};

struct cpu_mesh
{
    cpu_mesh()
        : numa(memory::MemoryAllocatorV)
        , topology_map(memory::MemoryAllocatorV, 10)
    {
    }
    freelibcxx::vector<numa_node> numa = memory::MemoryAllocatorV;
    freelibcxx::hash_map<int, freelibcxx::tuple<logic_core_id, logic_core>> topology_map;

    int logic_num = 0;
    int core_num = 0;

    int enabled_logic_num = 0;
};

void load_cpu_mesh(cpu_mesh &mesh);

u64 max_basic_cpuid();

const char *get_cpu_manufacturer();

} // namespace arch::cpu_info