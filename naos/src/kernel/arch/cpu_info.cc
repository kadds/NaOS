#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/trace.hpp"

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

int logic_bits()
{
    u32 eax, ebx, ecx, edx;
    cpu_id(0x1, 0, eax, ebx, ecx, edx);
    return bits(ebx, 16, 23);
}

cpu_mesh get_cpu_mesh_info_amd()
{
    cpu_mesh mesh;
    int logic = logic_bits();
    u32 eax, ebx, ecx, edx;
    cpu_id(0x80000008, 0, eax, ebx, ecx, edx);
    int core_num = bits(ecx, 0, 7) + 1;
    if (logic == 0)
    {
        logic = core_num;
    }
    mesh.logic_num = logic;
    mesh.core_num = core_num;
    return mesh;
}

cpu_mesh get_cpu_mesh_info_intel()
{
    cpu_mesh mesh;
    int logic = logic_bits();
    u32 eax, ebx, ecx, edx;
    cpu_id(0x4, 0, eax, ebx, ecx, edx);
    int core_num = bits(eax, 26, 31) + 1;
    if (logic == 0)
    {
        logic = core_num;
    }
    mesh.logic_num = logic;
    mesh.core_num = core_num;
    return mesh;
}

cpu_mesh get_cpu_mesh_info()
{
    if (family_name[0] == 'A')
    {
        return get_cpu_mesh_info_amd();
    }
    else
    {
        return get_cpu_mesh_info_intel();
    }
}

const char *get_cpu_manufacturer() { return family_name; }

} // namespace arch::cpu_info
