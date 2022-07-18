#pragma once
#include "common.hpp"
#include "kernel/util/memory.hpp"
#include <type_traits>

namespace arch::paging
{
struct flags
{
    enum : u64
    {
        present = 1,
        writable = 2,
        user_mode = 4,
        write_through = 8,
        cache_disable = 16,
        accessed = 32,
        dirty = 64,
        attribute_ext = 128,
        big_page = 128,
        global = 256,

        // arch flags
        uncacheable = 24,

    };
};

struct page_fault_flags
{
    enum : u64
    {
        // When set, the page fault was caused by a page-protection violation. When not set, it was caused by a
        // non-present page.
        present = 1,
        // When set, the page fault was caused by a write access. When not set, it was caused by a read access.
        write = 2,
        // 	When set, the page fault was caused while CPL = 3. This does not necessarily mean that the page fault was a
        // privilege violation.
        user = 4,
        // When set, one or more page directory entries contain reserved bits which are set to 1. This only applies when
        // the PSE or PAE flags in CR4 are set to 1.
        reserved_write = 8,
        // 	When set, the page fault was caused by an instruction fetch. This only applies when the No-Execute bit is
        // supported and enabled.
        instruction_fetch = 16,
        // When set, the page fault was caused by a protection-key violation. The PKRU register (for user-mode accesses)
        // or PKRS MSR (for supervisor-mode accesses) specifies the protection key rights.
        protection_key = 32,
        // When set, the page fault was caused by a shadow stack access.
        shadow_stack = 64,
    };
};

struct base_entry
{
    u64 data;
    bool is_present() { return data & flags::present; }
    bool is_writable() { return data & flags::writable; }
    bool is_user_mode() { return data & flags::user_mode; }
    bool is_write_through() { return data & flags::write_through; }
    bool is_cache_disable() { return data & flags::cache_disable; }
    bool is_accessed() { return data & flags::accessed; }
    bool is_dirty() { return data & flags::dirty; }
    bool is_big_page() { return data & flags::big_page; }
    bool is_global() { return data & flags::global; }

    void set_present() { data |= flags::present; }
    void clear_present() { data &= ~flags::present; }

    void set_writable() { data |= flags::writable; }
    void clear_writable() { data &= ~flags::writable; }

    void set_user_mode() { data |= flags::user_mode; }
    void clear_user_mode() { data &= ~flags::user_mode; }

    void set_write_through() { data |= flags::write_through; }
    void clear_write_through() { data &= ~flags::write_through; }

    void set_cache_disable() { data |= flags::cache_disable; }
    void clear_cache_disable() { data &= ~flags::cache_disable; }

    void clear_accessed() { data &= ~flags::accessed; }

    void clear_dirty() { data &= ~flags::dirty; }

    void set_big_page() { data |= flags::big_page; }
    void clear_big_page() { data &= ~flags::big_page; }

    void set_global() { data |= flags::global; }
    void clear_global() { data &= ~flags::global; }

    void *get_addr() const;
    void *get_phy_addr() const;

    base_entry(void *baseAddr, u32 flags) { data = (((u64)baseAddr) & 0x1F'FFFF'FFFF'F000UL) | (flags & 0x1FF); }
    base_entry() { data = 0; }
    void set_addr(void *ptr);
    void set_phy_addr(void *ptr);

    constexpr static u16 common_flag_share = 1024;

    // can only save 11 bits data
    void set_common_data(u16 dt)
    {
        dt &= 0x7FF;
        u64 d = dt;
        data &= 0x800F'FFFF'FFFF'F1FFUL;
        data |= d << 52;
    }

    u16 get_common_data()
    {
        u16 rt;
        rt = (((data) >> 52) & 0x7FF);
        return rt;
    }

    void add_counter()
    {
        u16 c = get_common_data();
        set_common_data(((c & 0x1FFU) + 1) | (c & ~0x1FFU));
    }
    void sub_counter()
    {
        u16 c = get_common_data();
        set_common_data(((c & 0x1FFU) - 1) | (c & ~0x1FFU));
    }
    u16 get_counter() { return get_common_data() & 0x1FFU; }
    void set_counter(u16 c) { set_common_data(((c & 0x1FFU)) | (get_common_data() & ~0x1FFU)); }

    void add_share_flag()
    {
        u16 c = get_common_data();
        set_common_data(c | 0x200U);
    }
    void clear_share_flag()
    {
        u16 c = get_common_data();
        set_common_data(c & ~0x200U);
    }
    bool has_share_flag()
    {
        u16 c = get_common_data();
        return c & 0x200U;
    }
};

struct pt_entry : base_entry
{
    static inline u64 page_size = 1 << 12;
    static inline u64 pages = 1 << 12;
    using base_entry::base_entry;
};
struct pt
{
    pt() { util::memzero(entries, sizeof(entries)); }
    pt_entry entries[512];
    pt_entry &operator[](size_t index) { return entries[index]; }
};

struct pd_entry : public base_entry
{
    static inline u64 big_page_size = 1 << 21;
    static inline u64 pages = 1 << 21;
    using base_entry::base_entry;

    pt &next() const { return *(pt *)get_addr(); }
};
struct pdt
{
    pdt() { util::memzero(entries, sizeof(entries)); }
    pd_entry entries[512];
    pd_entry &operator[](size_t index) { return entries[index]; }
};

struct pdpt_entry : public base_entry
{
    static inline u64 big_page_size = 1 << 30;
    static inline u64 pages = 1UL << 30;
    pdt &next() const { return *(pdt *)get_addr(); }
    using base_entry::base_entry;

  private:
    bool is_dirty();
    bool is_global();
    void clear_dirty();
    void set_global();
    void clear_global();
};
struct pdpt
{
    pdpt() { util::memzero(entries, sizeof(entries)); }
    pdpt_entry entries[512];
    pdpt_entry &operator[](size_t index) { return entries[index]; }
};

struct pml4_entry : public base_entry
{
    static inline u64 pages = 1UL << 39;
    pdpt &next() const { return *(pdpt *)get_addr(); }
    using base_entry::base_entry;
};

struct pml4t
{
    pml4t() { util::memzero(entries, sizeof(entries)); }
    pml4_entry entries[512];
    pml4_entry &operator[](size_t index) { return entries[index]; }
};
namespace frame_size
{
enum frame_size : u64
{
    size_4kb = 0x1000,
    size_2mb = 0x200000,
    size_1gb = 0x40000000
};
} // namespace frame_size
using base_paging_t = pml4t;

bool map(base_paging_t *base_paging_addr, void *virt_start_addr, phy_addr_t phy_start_addr, u64 frame_size,
         u64 frame_count, u32 page_ext_flags, bool override);
bool unmap(base_paging_t *base_paging_addr, void *virt_start_addr, u64 frame_size, u64 frame_count);

void load(base_paging_t *base_paging_addr);
void reload();

void init();
void enable_new_paging();
void temp_init(bool is_bsp);

void share_page_table(base_paging_t *to, base_paging_t *source, u64 start, u64 end, bool override, int deep);
void sync_kernel_page_table(base_paging_t *to, base_paging_t *kernel);

bool has_map(base_paging_t *base_paging_addr, void *virt_addr);

bool has_shared(base_paging_t *base, void *vir);
bool clear_shared(base_paging_t *base, void *vir, bool writeable);

/// get phy addr
bool get_map_address(base_paging_t *base_paging_addr, void *virt_addr, phy_addr_t *phy_addr);

base_paging_t *current();

} // namespace arch::paging
