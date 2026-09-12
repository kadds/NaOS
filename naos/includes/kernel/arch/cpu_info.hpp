#pragma once
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/common.hpp"
#include "kernel/mm/new.hpp"

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
    erms,
    fsrm,
    rdseed,
    rdrand,

    crystal_frequency,
    tsc_frequency,
    cpu_base_frequency,
    cpu_max_frequency,
    bus_frequency,
};

/// Convert CPUID.15H's ratio to an exact integer Hertz value without 32-bit
/// overflow. A zero result means that the leaf did not provide a usable ratio.
constexpr u64 tsc_frequency_from_cpuid15(u32 denominator, u32 numerator, u32 crystal_frequency_hz) noexcept
{
    if (denominator == 0 || numerator == 0 || crystal_frequency_hz == 0)
        return 0;
    return static_cast<u64>(crystal_frequency_hz) * numerator / denominator;
}

/// Raw CPUID.15H TSC/Crystal Clock Information.
struct tsc_cpuid15_info
{
    bool leaf_available = false;
    u32 denominator = 0;
    u32 numerator = 0;
    u32 crystal_frequency_hz = 0;

    constexpr bool has_frequency() const noexcept
    {
        return leaf_available && denominator != 0 && numerator != 0 && crystal_frequency_hz != 0;
    }

    constexpr u64 frequency_hz() const noexcept
    {
        return tsc_frequency_from_cpuid15(denominator, numerator, crystal_frequency_hz);
    }
};

void init();
/// check if has the feature
bool has_feature(feature f);

tsc_cpuid15_info get_tsc_cpuid15_info();

/// Try to read one hardware-random word. The functions return false when the
/// instruction is unavailable or its entropy source is temporarily empty.
bool try_rdseed(u64 &value);
bool try_rdrand(u64 &value);

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
