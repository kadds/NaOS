#pragma once
#include "kernel/common.hpp"

namespace arch::tss
{

struct descriptor
{
    u16 limit0;
    u16 offset0;
    u8 offset1;
    u8 type : 4;
    u8 system : 1;
    u8 DPL : 2;
    u8 P : 1;
    u8 limit1 : 4;
    u8 AVL : 1;
    u8 : 0;
    u8 offset2;
    u32 offset3;
    u32 none;
    descriptor()
    {
        *((u64 *)this) = 0;
        *((u64 *)this + 1) = 0;
    }
    u64 offset() { return ((u64)offset3) << 32 | ((u64)offset2 << 24) | ((u64)offset1) << 16 | (u64)offset0; }
    void set_offset(u64 offset)
    {
        offset3 = offset >> 32;
        offset2 = offset >> 24;
        offset1 = offset >> 16;
        offset0 = offset;
    }

    u32 limit() { return ((u32)limit0) | ((u32)limit1) << 16; }
    void set_limit(u32 limit)
    {
        limit0 = limit;
        limit1 = limit >> 16;
    }
    void set_type(u8 type) { this->type = type; }
    void set_dpl(u8 dpl) { this->DPL = dpl; }
    void set_valid(bool p) { this->P = p; }
} PackStruct;
static_assert(sizeof(descriptor) == 16);

struct tss_t
{
    u32 _0;
    u32 rsp0_low;
    u32 rsp0_high;
    u32 rsp1_low;
    u32 rsp1_high;
    u32 rsp2_low;
    u32 rsp2_high;
    u64 _1;
    u32 ist1_low;
    u32 ist1_high;
    u32 ist2_low;
    u32 ist2_high;
    u32 ist3_low;
    u32 ist3_high;
    u32 ist4_low;
    u32 ist4_high;
    u32 ist5_low;
    u32 ist5_high;
    u32 ist6_low;
    u32 ist6_high;
    u32 ist7_low;
    u32 ist7_high;
    u64 _2;
    u16 _3;
    u16 io_address;
} PackStruct;

static_assert(sizeof(tss_t) == 104, "sizeof(tss_t) must == 104");

/// x86 requires one permission bit for every I/O port and one terminating
/// 0xff byte after the bitmap.  The bitmap is per CPU and is never shared
/// between address spaces.
inline constexpr u32 io_bitmap_bytes = 8192;
inline constexpr u32 io_bitmap_guard_bytes = 1;
inline constexpr u32 max_direct_io_ranges = 16;

struct io_range
{
    u16 first_port;
    u32 port_count;
};

struct alignas(16) tss_storage
{
    tss_t tss;
    u8 io_bitmap[io_bitmap_bytes + io_bitmap_guard_bytes];
    u32 cached_io_range_count = 0;
    io_range cached_io_ranges[max_direct_io_ranges]{};
};

static_assert(sizeof(tss_storage) >= sizeof(tss_t) + io_bitmap_bytes + io_bitmap_guard_bytes);

void init(int core_index, phy_addr_t base_addr, void *ist);

void set_ist(int core_index, int index, void *ist);
void *get_ist(int core_index, int index);

void set_rsp(int core_index, int index, void *rsp0);
void *get_rsp(int core_index, int index);

/// Install one contiguous, direct-I/O lease on a CPU's current TSS.  This is
/// a kernel-internal data-plane primitive; capability admission and thread
/// ownership are enforced by the caller before reaching it.
bool install_io_lease(int core_index, u16 first_port, u32 port_count);

/// Rebuild the bitmap from the complete lease set of the thread being
/// scheduled. The set is bounded by the task layer and is never shared with
/// another address space.
bool install_io_leases(int core_index, const io_range *ranges, u32 range_count);

/// Revoke the current direct-I/O lease and put the TSS back in explicit
/// all-deny mode.  The caller must stop the affected user thread before
/// changing its effective lease.
void revoke_io_lease(int core_index);
} // namespace arch::tss
