#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/trace.hpp"
#define ret_cpu_feature(number, reg, bit)                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((number > max_basic_number && number < 0x80000000) || number > max_extend_number)                          \
        {                                                                                                              \
            trace::info("cpu feature not found number: ", number);                                                              \
            return false;                                                                                              \
        }                                                                                                              \
        cpu_id(number, &eax, &ebx, &ecx, &edx);                                                                        \
        return (reg & 1ul << bit);                                                                                     \
    } while (0)

#define bits(number, start, end) ((number) >> (start) & ((1 << (end - start + 1)) - 1))

namespace arch::cpu_info
{
char cpu_name[13];
u32 max_basic_number;
u32 max_extend_number;

void trace_debug_info();

void init()
{
    u32 a, b, c, d;
    cpu_id(0, &a, &b, &c, &d);
    *(u32 *)cpu_name = b;
    *((u32 *)cpu_name + 1) = d;
    *((u32 *)cpu_name + 2) = c;
    cpu_name[12] = '\0';
    max_basic_number = a;

    cpu_id(0x80000000, &a, &b, &c, &d);
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
            trace::panic("Unsupported feature");
        }
    }
}

void trace_debug_info()
{
    trace::debug("Cpu family: ", cpu_name, ". Maximum basic functional number: ", (void *)(u64)max_basic_number,
                 ". Maximum extend functional number: ", (void *)(u64)max_extend_number,
                 ". Maximum virtual address bits ", get_feature(feature::max_virt_addr),
                 ". Maximum physical address bits ", get_feature(feature::max_phy_addr));
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
        case feature::contant_tsc:
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
            cpu_id(0x80000008, &eax, &ebx, &ecx, &edx);
            return bits(eax, 0, 7);
        case feature::max_virt_addr:
            cpu_id(0x80000008, &eax, &ebx, &ecx, &edx);
            return bits(eax, 8, 15);
        default:
            trace::panic("Unknown feature");
    }
}

const char *get_cpu_manufacturer() { return cpu_name; }

} // namespace arch::cpu_info
