#include "kernel/arch/cpu_info.hpp"
#include "freelibcxx/algorithm.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/tuple.hpp"
#include "freelibcxx/utils.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/trace.hpp"

#define CPUID_VENDOR_AMD "AuthenticAMD"
#define CPUID_VENDOR_INTEL "GenuineIntel"

void cpu_id(u32 fn, u32 p, u32 &eax, u32 &ebx, u32 &ecx, u32 &edx) {
    eax = fn;
    ecx = p;
    _cpu_id(&eax, &ebx, &ecx, &edx);
}

#define ret_cpu_feature(number, reg, bit)                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((number > max_basic_number && number < 0x80000000) || number > max_extend_number)                          \
        {                                                                                                              \
            trace::info("cpu feature not found number: ", number);                                                     \
            return false;                                                                                              \
        }                                                                                                              \
        cpu_id(number, 0, eax, ebx, ecx, edx);                                                                                \
        return (reg & 1ul << bit);                                                                                     \
    } while (0)

#define bits(number, start, end) ((number) >> (start) & ((1 << (end - start + 1)) - 1))

namespace arch::cpu_info
{
char family_name[13];
char brand_name[64];
u32 max_basic_number;
u32 max_extend_number;
bool intel_cpu = false;

void trace_debug_info();
u64 max_basic_cpuid() { return max_basic_number; }

void load_brand_name()
{
    u32 a, b, c, d;
    cpu_id(0x80000002, 0, a, b, c, d);
    *(u32 *)brand_name = a;
    *((u32 *)brand_name + 1) = b;
    *((u32 *)brand_name + 2) = c;
    *((u32 *)brand_name + 3) = d;
    cpu_id(0x80000003, 0, a, b, c, d);
    *((u32 *)brand_name + 4) = a;
    *((u32 *)brand_name + 5) = b;
    *((u32 *)brand_name + 6) = c;
    *((u32 *)brand_name + 7) = d;
    cpu_id(0x80000004, 0, a, b, c, d);
    *((u32 *)brand_name + 8) = a;
    *((u32 *)brand_name + 9) = b;
    *((u32 *)brand_name + 10) = c;
    *((u32 *)brand_name + 11) = d;
}

void init()
{
    u32 a, b, c, d;
    cpu_id(0, 0, a, b, c, d);
    *(u32 *)family_name = b;
    *((u32 *)family_name + 1) = d;
    *((u32 *)family_name + 2) = c;
    family_name[12] = '\0';
    max_basic_number = a;
    load_brand_name();

    cpu_id(0x80000000, 0, a, b, c, d);
    max_extend_number = a;
    trace_debug_info();
    feature required_feature[] = {
        feature::system_call_ret,
        feature::msr,
        feature::tsc,
        feature::apic,
    };
    for (auto feat : required_feature)
    {
        if (!has_feature(feat))
        {
            trace::panic("Unsupported feature ", (int)feat);
        }
    }
    intel_cpu = strstr(family_name, CPUID_VENDOR_INTEL) != nullptr;
}

void trace_debug_info()
{
    trace::debug("Cpu family: ", family_name, ". Cpu name: ", brand_name,
                 ".\n    Maximum basic functional number: ", (void *)(u64)max_basic_number,
                 ". Maximum extend functional number: ", (void *)(u64)max_extend_number,
                 ".\n    Maximum virtual address bits ", get_feature(feature::max_virt_addr),
                 ". Maximum physical address bits ", get_feature(feature::max_phy_addr));

    if (max_basic_number >= 0x16)
    {
        trace::debug("cpu base frequency ", get_feature(feature::cpu_base_frequency), "MHZ", " max frequency ",
                     get_feature(feature::cpu_max_frequency), "MHZ", " bus frequency ",
                     get_feature(feature::bus_frequency), "MHZ");
    }
}

bool has_feature(feature f)
{
    u32 eax, ebx, ecx, edx;
    switch (f)
    {
        case feature::system_call_ret:
            ret_cpu_feature(0x80000001, edx, 11);
        case feature::pcid:
            ret_cpu_feature(0x1, ecx, 17);
        case feature::fpu:
            ret_cpu_feature(0x1, edx, 0);
        case feature::huge_page_1gb:
            ret_cpu_feature(0x80000001, edx, 26);
        case feature::apic:
            ret_cpu_feature(0x1, edx, 9);
        case feature::xapic:
            ret_cpu_feature(0x1, edx, 9);
        case feature::x2apic:
            ret_cpu_feature(0x1, ecx, 21);
        case feature::msr:
            ret_cpu_feature(0x1, edx, 5);
        case feature::tsc:
            ret_cpu_feature(0x1, edx, 4);
        case feature::constant_tsc:
            ret_cpu_feature(0x80000007, edx, 8);
        case feature::sse:
            ret_cpu_feature(0x1, edx, 25);
        case feature::sse2:
            ret_cpu_feature(0x1, edx, 26);
        case feature::sse3:
            ret_cpu_feature(0x1, ecx, 0);
        case feature::ssse3:
            ret_cpu_feature(0x1, ecx, 9);
        case feature::sse4_1:
            ret_cpu_feature(0x1, ecx, 19);
        case feature::sse4_2:
            ret_cpu_feature(0x1, ecx, 20);
        case feature::popcnt_i:
            ret_cpu_feature(0x1, ecx, 23);
        case feature::htt:
            ret_cpu_feature(0x1, edx, 28);
        case feature::xsave:
            ret_cpu_feature(0x1, ecx, 26);
        case feature::osxsave:
            ret_cpu_feature(0x1, ecx, 27);
        case feature::avx:
            ret_cpu_feature(0x1, ecx, 28);
        default:
            trace::panic("Unknown feature");
    }
}

u64 get_feature(feature f)
{
    u32 eax, ebx, ecx, edx;
    switch (f)
    {
        case feature::max_phy_addr:
            cpu_id(0x80000008, 0, eax, ebx, ecx, edx);
            return bits(eax, 0, 7);
        case feature::max_virt_addr:
            cpu_id(0x80000008, 0, eax, ebx, ecx, edx);
            return bits(eax, 8, 15);
        case feature::crystal_frequency:
            cpu_id(0x15, 0, eax, ebx, ecx, edx);
            return ecx; // hz
        case feature::tsc_frequency:
            cpu_id(0x15, 0, eax, ebx, ecx, edx);
            if (eax != 0 && ebx != 0)
            {
                return ecx * ebx / eax;
            }
            return 0;
        case feature::cpu_base_frequency:
            cpu_id(0x16, 0, eax, ebx, ecx, edx);
            return eax & 0xFFFF; // mhz
        case feature::cpu_max_frequency:
            cpu_id(0x16, 0, eax, ebx, ecx, edx);
            return ebx & 0xFFFF; // mhz
        case feature::bus_frequency:
            cpu_id(0x16, 0, eax, ebx, ecx, edx);
            return ecx & 0xFFFF; // mhz
        default:
            trace::panic("Unknown feature");
    }
}

int apic_id()
{
    u32 eax, ebx, ecx, edx;
    cpu_id(0x1, 0, eax, ebx, ecx, edx);
    return bits(ebx, 24, 31);
}

int logic_counter()
{
    u32 eax, ebx, ecx, edx;
    cpu_id(0x1, 0, eax, ebx, ecx, edx);
    return bits(ebx, 16, 23);
}

int count_bits(int val)
{
    int n = 0;
    while (val != 0)
    {
        val <<= 1;
        n++;
    }
    return n;
}
int count_lbits(int val)
{
    int n = 0;
    while (val != 0)
    {
        val >>= 1;
        n++;
    }
    return n;
}

freelibcxx::tuple<int, int> get_amd_apic_bits()
{
    u32 eax, ebx, ecx, edx;
    int core_bits = 0, logic_bits = 0;
    if (max_extend_number >= 0x8000'0008)
    {
        cpu_id(0x80000008, 0, eax, ebx, ecx, edx);
        core_bits = bits(ecx, 12, 15);
        if (core_bits == 0)
        {
            core_bits = bits(ecx, 0, 7);
            core_bits = freelibcxx::next_pow_of_2(core_bits);
        }

        int logic_count = logic_counter();

        logic_count >>= core_bits;
        logic_bits = freelibcxx::next_pow_of_2(logic_count);
    }
    else
    {
        int logic_count = logic_counter();
        core_bits = freelibcxx::next_pow_of_2(logic_count);
    }

    return freelibcxx::make_tuple(core_bits, logic_bits);
}
freelibcxx::tuple<int, int> get_intel_apic_bits()
{
    u32 eax, ebx, ecx, edx;
    if (max_basic_number >= 0xB)
    {
        cpu_id(0xB, 0, eax, ebx, ecx, edx);
        int logic_bits = bits(eax, 0, 4);
        cpu_id(0xB, 1, eax, ebx, ecx, edx);
        int tmp = bits(eax, 0, 4);
        return freelibcxx::make_tuple(tmp - logic_bits, logic_bits);
    }
    else if (max_basic_number >= 0x4)
    {
        int logic_count = logic_counter();
        cpu_id(0x4, 0, eax, ebx, ecx, edx);
        int apic_chip_bits = bits(eax, 26, 31);
        int max_cores_on_chip = logic_count & 0x3F;
        int core_bits = count_bits(max_cores_on_chip);
        return freelibcxx::make_tuple(core_bits, count_bits(apic_chip_bits - 1) - core_bits);
    }
    else
    {
        return freelibcxx::make_tuple(0, 0);
    }
}

freelibcxx::tuple<int, int> get_apic_bits()
{
    if (!has_feature(feature::htt))
    {
        return freelibcxx::make_tuple(0, 0);
    }
    if (intel_cpu)
    {
        return get_intel_apic_bits();
    }
    else
    {
        return get_amd_apic_bits();
    }
}

const char *get_cpu_manufacturer() { return family_name; }

void load_cpu_mesh(cpu_mesh &mesh)
{
    int config_cpu_count = cmdline::get_int("cpu_num", 0);
    auto [core_bits, logic_bits] = get_apic_bits();
    int chip_bits = core_bits + logic_bits;
    trace::debug("cpu core bits ", core_bits, " logic bits ", logic_bits);

    if (ACPI::has_init())
    {
        auto list = ACPI::get_local_apic_list();
        auto numa_map = ACPI::get_numa_info();
        for (auto &lapic_info : list)
        {
            if (!lapic_info.enabled)
            {
                trace::info("cpu ", lapic_info.apic_id, " disabled");
            }
            auto numa_info = numa_map.get(lapic_info.apic_id);
            logic_core_id id;
            if (numa_info.has_value())
            {
                auto &numa = numa_info.value();
                if (numa.enabled)
                {
                    // TODO: chip_index should be corrected
                    id.chip_index = lapic_info.apic_id & ~((1 << chip_bits) - 1);
                    id.numa_index = numa.domain;
                }
                else
                {
                    continue;
                }
            }
            else
            {
                id.chip_index = lapic_info.apic_id >> chip_bits;
            }
            id.logic_index = lapic_info.apic_id & ((1 << logic_bits) - 1);
            id.core_index = (lapic_info.apic_id >> logic_bits) & ((1 << core_bits) - 1);
            logic_core logic;
            logic.enabled = lapic_info.enabled;
            logic.apic_id = lapic_info.apic_id;
            logic.apic_process_id = lapic_info.processor_id;
            logic.exist = true;

            mesh.topology_map.insert(lapic_info.apic_id, freelibcxx::make_tuple(id, logic));
        }
    }
    else
    {
        // configure core nums
        if (config_cpu_count <= 0)
        {
            config_cpu_count = 1;
        }
        for (int i = 0; i < config_cpu_count; i++)
        {
            logic_core_id id;
            id.core_index = i;

            logic_core logic;
            logic.apic_id = i;
            logic.apic_process_id = i;
            logic.enabled = true;
            logic.exist = true;
            mesh.topology_map.insert(i, freelibcxx::make_tuple(id, logic));
        }
    }

    // build mesh

    for (auto &item : mesh.topology_map)
    {
        auto [id, logic_core_info] = item.value;
        if (mesh.numa.size() <= id.numa_index)
        {
            mesh.numa.resize(id.numa_index + 1, numa_node());
        }
        auto &numa = mesh.numa[id.numa_index];
        if (numa.chip.size() <= id.chip_index)
        {
            numa.chip.resize(id.chip_index + 1, chip_node());
        }
        auto &chip = numa.chip[id.chip_index];
        if (chip.cores.size() <= id.core_index)
        {
            chip.cores.resize(id.core_index + 1, core());
        }
        auto &core = chip.cores[id.core_index];
        if (core.logics.size() <= id.logic_index)
        {
            core.logics.resize(id.logic_index + 1, logic_core());
        }
        auto &logic = core.logics[id.logic_index];
        logic = logic_core_info;
    }

    // calculate cores
    for (auto &numa : mesh.numa)
    {
        for (auto &chip : numa.chip)
        {
            for (auto &core : chip.cores)
            {
                bool core_enabled = false;
                for (auto &logic : core.logics)
                {
                    if (logic.enabled && logic.exist)
                    {
                        if (config_cpu_count > 0 && mesh.logic_num >= config_cpu_count)
                        {
                            logic.enabled = false;
                            break;
                        }
                        mesh.logic_num++;
                        core_enabled = true;
                    }
                }
                if (core_enabled)
                {
                    mesh.core_num++;
                }
            }
        }
    }
}

} // namespace arch::cpu_info
