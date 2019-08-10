#pragma once
#include "common.hpp"
#include <type_traits>

namespace paging
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

    void *get_addr() const { return (void *)((u64)data & 0x1FFFFFFFFFF000UL); }
    base_entry(void *baseAddr, u32 flags) { data = ((((u64)baseAddr) & 0x1FFFFFFFFFF000UL) | flags); }
    base_entry() { data = 0; }
    void set_addr(void *ptr) { data = ((u64)ptr) & 0x1FFFFFFFFFF000UL; }
    void add_attributes(u16 attr) { data &= (attr & 0x0FFF); }
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

    pt &next() const { return *(pt *)((u64)data & 0x1FFFFFFFFFF000UL); }
};
struct pdt
{
    pd_entry entries[512];
    pd_entry &operator[](size_t index) { return entries[index]; }
};

struct pdpt_entry : public base_entry
{
    static inline u64 big_page_size = 1 << 30;
    pdt &next() const { return *(pdt *)((u64)data & 0x1FFFFFFFFFF000UL); }
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
    pdpt &next() const { return *(pdpt *)((u64)data & 0x1FFFFFFFFFF000UL); }
    using base_entry::base_entry;
};

struct pml4t
{
    pml4_entry entries[512];
    pml4_entry &operator[](size_t index) { return entries[index]; }
};

enum class frame_size_type : u64
{
    size_4_kb = 0x1000,
    size_2_mb = 0x200000,
    size_1_gb = 0x80000000
};

extern pml4t *base_page_addr;
bool map(void *virt_start_addr, void *phy_start_addr, frame_size_type frame_size, u32 map_count, u32 page_ext_flags);

void init();
void temp_init();
static_assert(sizeof(pml4t) == 0x1000 && sizeof(pdpt) == 0x1000 && sizeof(pdt) == 0x1000 && sizeof(pt) == 0x1000,
              "sizeof paging struct is not 4KB.");
struct page
{
    u64 attr;
};

} // namespace paging
