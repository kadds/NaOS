#pragma once
#include "common.hpp"
#include <type_traits>
/*
kernel map
0xFFFF,8000,0000,0000 - 0xFFFF,8A00,0000,0000: 32TB  : kernel fixed memory map
0xFFFF,FFFF,0000,0000 - 0xFFFF,FFFF,F000,FFFF: 256MB : pre cpu kernel data
0xFFFF,E000,F000,0000 - 0xFFFF,E000,FFFF,FFFF: video map
0xFFFF,D000,0000,0000 - 0xFFFF,DFFF,FFFF,FFFF:
*/
namespace arch::paging
{
struct flags
{
    enum : u16
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
    base_entry(void *baseAddr, u32 flags) { data = (((u64)baseAddr) & 0x1FFFFFFFFFF000UL) | (flags & 0x1FF); }
    base_entry() { data = 0; }
    void set_addr(void *ptr);
    void add_attributes(u16 attr) { data &= (attr & 0x1FF); }
    // can only save 14 bits data
    void set_common_data(u16 dt)
    {
        dt &= 0x3FFF;
        data |= (((u64)dt & 0x3FFC) << 50) & ((dt & 0x3000) >> 3);
    }
    u16 get_common_data()
    {
        u16 rt;
        rt = (((data) >> 9) & 0x2) | ((((data) >> 52) & 0xFFF) << 2);
        return rt;
    }
    // can only save 62 bits data, notice: will clean common data
    void set_unpresent_data(u64 dt)
    {
        dt <<= 2;
        data |= dt;
    }
    u64 get_unpresent_data() { return (data & 0x7FFFFFFFFFFFFFFEUL) >> 1; }
};

struct pt_entry : base_entry
{
    static inline u64 page_size = 1 << 12;
    using base_entry::base_entry;
};
struct pt
{
    pt_entry entries[512];
    pt_entry &operator[](size_t index) { return entries[index]; }
};

struct pd_entry : public base_entry
{
    static inline u64 big_page_size = 1 << 21;
    using base_entry::base_entry;

    pt &next() const { return *(pt *)get_addr(); }
};
struct pdt
{
    pd_entry entries[512];
    pd_entry &operator[](size_t index) { return entries[index]; }
};

struct pdpt_entry : public base_entry
{
    static inline u64 big_page_size = 1 << 30;
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
    pdpt_entry entries[512];
    pdpt_entry &operator[](size_t index) { return entries[index]; }
};

struct pml4_entry : public base_entry
{
    pdpt &next() const { return *(pdpt *)get_addr(); }
    using base_entry::base_entry;
};

struct pml4t
{
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

bool map(base_paging_t *base_paging_addr, void *virt_start_addr, void *phy_start_addr, u64 frame_size, u64 frame_count,
         u32 page_ext_flags);
bool unmap(base_paging_t *base_paging_addr, void *virt_start_addr, u64 frame_size, u64 frame_count);

void load(base_paging_t *base_paging_addr);
void reload();

void init();
void temp_init();

void copy_page_table(base_paging_t *to, base_paging_t *source, u64 start, u64 end, bool override);
void sync_kernel_page_table(base_paging_t *to, base_paging_t *kernel);

/// get phy addr
bool get_map_address(base_paging_t *base_paging_addr, void *virt_addr, void **phy_addr);

base_paging_t *current();

} // namespace arch::paging
