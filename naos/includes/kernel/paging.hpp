#pragma once
#include "common.hpp"

namespace paging
{
struct pte
{
    enum class flag
    {
        present = 1,
        read_write = 2,
        user_supervisor = 4,
        page_write_through = 8,
        page_cache_disable = 16,
        accessed = 32,
        dirty = 64,
        global = 256,
    };
    static inline u64 page_size = 1 << 12;
    void *data;
    void *get_addr() const { return (void *)((u64)data & 0x1FFFFFFFFFF000UL); } // 4KB
    pte(void *baseAddr, u32 flags) { data = (void *)((((u64)baseAddr) & 0x1FFFFFFFFFF000UL) | flags); }
    pte() { data = 0; }
};
struct pde
{
    enum flag
    {
        present = 1,
        read_write = 2,
        user_supervisor = 4,
        page_write_through = 8,
        page_cache_disable = 16,
        accessed = 32,
        dirty = 64,
        big_page = 128,
        global = 256,
    };
    static inline u64 big_page_size = 1 << 21;
    void *data;
    void *get_addr() const { return (void *)((u64)data & 0x1FFFFFFFE00000UL); } // 2MB
    pte *get_next() const { return (pte *)((u64)data & 0x1FFFFFFFFFF000UL); }
    bool is_big_page() const { return (u64)data & big_page; }
    pde(void *baseAddr, u32 flags) { data = (void *)((((u64)baseAddr) & 0x1FFFFFFFFFF000UL) | flags); }
    pde() { data = 0; }
};
struct pdpte
{
    enum flag
    {
        present = 1,
        read_write = 2,
        user_supervisor = 4,
        page_write_through = 8,
        page_cache_disable = 16,
        accessed = 32,
        dirty = 64,
        big_page = 128,
        global = 256,
    };
    static inline u64 big_page_size = 1 << 30;
    void *data;
    void *get_addr() const { return (void *)((u64)data & 0x1FFFFFC0000000UL); } // 1GB
    pde *get_next() const { return (pde *)((u64)data & 0x1FFFFFFFFFF000UL); }
    bool is_big_page() const { return ((u64)data) & big_page; }
    pdpte(void *baseAddr, u32 flags) { data = (void *)((((u64)baseAddr) & 0x1FFFFFFFFFF000UL) | flags); }
    pdpte() { data = 0; }
};
#define pml4e_flag_Enable 1

struct pml4e
{
    enum flag
    {
        present = 1,
        read_write = 2,
        user_supervisor = 4,
        page_write_through = 8,
        page_cache_disable = 16,
        accessed = 32,
    };
    void *data;
    pdpte *get_next() const { return (pdpte *)((u64)data & 0x1FFFFFFFFFF000UL); }
    pml4e(void *baseAddr, u32 flags) { data = (void *)((((u64)baseAddr) & 0x1FFFFFFFFFF000UL) | flags); }
    pml4e() { data = 0; }
};

extern pml4e *page_addr;

void init();
} // namespace paging
