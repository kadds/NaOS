#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/trace.hpp"
#define ret_cpu_future(number, reg, bit)                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((number > max_basic_number && number < 0x80000000) || number > max_extend_number)                          \
            return false;                                                                                              \
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
    future min_future[] = {
        future::system_call_ret,
        future::msr,
        future::tsc,
    };
    for (auto fut : min_future)
    {
        if (!has_future(fut))
        {
            trace::panic("Unsupported future.");
        }
    }
}

void trace_debug_info()
{
    trace::debug("Cpu family: ", cpu_name, ".\nMaximum basic functional number: ", (void *)(u64)max_basic_number,
                 ".\nMaximum extend functional number: ", (void *)(u64)max_extend_number,
                 ".\nMaximum virtual address bits ", get_future(future::max_virt_addr),
                 ".\nMaximum physical address bits ", get_future(future::max_phy_addr));
}

bool has_future(future f)
{
    u32 eax, ebx, ecx, edx;
    switch (f)
    {
        case future::system_call_ret:
            ret_cpu_future(0x80000001, edx, 11);
        case future::pcid:
            ret_cpu_future(0x1, ecx, 17);
        case future::fpu:
            ret_cpu_future(0x1, edx, 0);
        case future::huge_page_1gb:
            ret_cpu_future(0x80000001, edx, 26);
        case future::apic:
            ret_cpu_future(0x1, edx, 9);
        case future::xapic:
            ret_cpu_future(0x1, edx, 9);
        case future::x2apic:
            ret_cpu_future(0x1, ecx, 21);
        case future::msr:
            ret_cpu_future(0x1, edx, 5);
        case future::tsc:
            ret_cpu_future(0x1, edx, 4);
        case future::contant_tsc:
            ret_cpu_future(0x80000007, edx, 8);
        case future::sse:
            ret_cpu_future(0x1, edx, 25);
        case future::sse2:
            ret_cpu_future(0x1, edx, 26);
        case future::sse3:
            ret_cpu_future(0x1, ecx, 0);
        case future::ssse3:
            ret_cpu_future(0x1, ecx, 9);
        case future::sse4_1:
            ret_cpu_future(0x1, ecx, 19);
        case future::sse4_2:
            ret_cpu_future(0x1, ecx, 20);
        case future::popcnt_i:
            ret_cpu_future(0x1, ecx, 23);
        default:
            trace::panic("Unknown future");
    }
}

u64 get_future(future f)
{
    u32 eax, ebx, ecx, edx;
    switch (f)
    {
        case future::max_phy_addr:
            cpu_id(0x80000008, &eax, &ebx, &ecx, &edx);
            return bits(eax, 0, 7);
        case future::max_virt_addr:
            cpu_id(0x80000008, &eax, &ebx, &ecx, &edx);
            return bits(eax, 8, 15);
        default:
            trace::panic("Unknown future");
    }
}

const char *get_cpu_manufacturer() { return cpu_name; }

} // namespace arch::cpu_info
