#pragma once
#include "common.hpp"
#include "freelibcxx/optional.hpp"
#include "freelibcxx/tuple.hpp"
#include "kernel/trace.hpp"
#include <cstddef>
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
        huge_page = 128,
        global = 256,
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
    void set_flags(u64 flags) { data = (data & 0xFFFF'FFFF'FFFF'F000UL) | (flags & 0xFFUL); }
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
    phy_addr_t get_phy_addr() const;

    base_entry(void *baseAddr, u32 flags) { data = (((u64)baseAddr) & 0x1F'FFFF'FFFF'F000UL) | (flags & 0x1FF); }
    base_entry() { data = 0; }
    void set_addr(void *ptr);
    void set_phy_addr(void *ptr);

    u64 flags() const { return data & 0xFFU; }
};

struct pt_entry : base_entry
{
    static inline u64 page_size = 1 << 12;
    static inline u64 pages = 1 << 12;
    using base_entry::base_entry;
};
struct pt
{
  private:
    pt_entry entries[512];

  public:
    pt() { memset(entries, 0, sizeof(entries)); }
    pt_entry &operator[](size_t index) { return entries[index]; }
    constexpr bool is_page_table() { return true; }
};

struct pd_entry : public base_entry
{
    static inline u64 big_page_size = 1 << 21;
    static inline u64 pages = 1 << 21;
    using base_entry::base_entry;

    pt &next() const { return *(pt *)get_addr(); }
    void set_next(pt *p) { set_addr(p); }
    pt_entry &operator[](size_t index) { return next()[index]; }
};
struct pdt
{
  private:
    pd_entry entries[512];

  public:
    pdt() { memset(entries, 0, sizeof(entries)); }
    pd_entry &operator[](size_t index) { return entries[index]; }
    constexpr bool is_page_table() { return true; }
};

struct pdpt_entry : public base_entry
{
    static inline u64 big_page_size = 1 << 30;
    static inline u64 pages = 1UL << 30;
    pdt &next() const { return *(pdt *)get_addr(); }
    void set_next(pdt *p) { set_addr(p); }
    pd_entry &operator[](size_t index) { return next()[index]; }
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
  private:
    pdpt_entry entries[512];

  public:
    pdpt() { memset(entries, 0, sizeof(entries)); }
    pdpt_entry &operator[](size_t index) { return entries[index]; }
    constexpr bool is_page_table() { return true; }
};

struct pml4_entry : public base_entry
{
    static inline u64 pages = 1UL << 39;
    pdpt &next() const { return *(pdpt *)get_addr(); }
    void set_next(pdpt *p) { set_addr(p); }
    pdpt_entry &operator[](size_t index) { return next()[index]; }
    using base_entry::base_entry;
};

struct pml4t
{
  private:
    pml4_entry entries[512];

  public:
    pml4t() { memset(entries, 0, sizeof(entries)); }
    pml4_entry &operator[](size_t index) { return entries[index]; }
    constexpr bool is_page_table() { return true; }
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

constexpr inline u64 big_pages = frame_size::size_2mb / frame_size::size_4kb;
constexpr inline u64 huge_pages = frame_size::size_1gb / frame_size::size_4kb;

void init();
void init_ap();
void enable_new_paging();
void temp_init(bool is_bsp);
void temp_update_uncached(void *virt, u64 pages);

namespace action_flags
{
enum
{
    override = 1,
    cow = 2,
    copy_all = 4,
};
}

class page_table_t
{
    struct index_t
    {
        u64 index;

        index_t()
            : index(0)
        {
        }
        index_t(u64 val)
            : index(val)
        {
        }

        index_t &operator++()
        {
            index++;
            return *this;
        }
        index_t operator++(int) const
        {
            auto new_index(index + 1);
            return new_index;
        }
        ptrdiff_t operator-(index_t rhs) const { return index - rhs.index; }

        freelibcxx::tuple<int, int, int, int> unpack()
        {
            u64 val = index;
            return freelibcxx::make_tuple((val >> 27) & 0x1FF, (val >> 18) & 0x1FF, (val >> 9) & 0x1FF, val & 0x1FF);
        }

        static index_t from_pack(int pml4e_index, int pdpe_index, int pde_index, int pte_index)
        {
            u64 index;
            index = pml4e_index;
            index <<= 9;
            index |= pdpe_index;
            index <<= 9;
            index |= pde_index;
            index <<= 9;
            index |= pte_index;
            return index_t(index);
        }
    };

  public:
    page_table_t();
    page_table_t(pml4t *base)
        : base_(base)
        , self_(false)
    {
    }
    page_table_t(const page_table_t &rhs) = delete;
    page_table_t &operator=(const page_table_t &rhs) = delete;
    page_table_t(page_table_t &&rhs)
        : base_(rhs.base_)
        , self_(rhs.self_)
    {
        rhs.base_ = nullptr;
    }
    page_table_t &operator=(page_table_t &&rhs);

    ~page_table_t();

    void load();
    static void reload();

    void map(void *virt_start, size_t pages, u64 flags, u64 actions);
    void map_to(void *virt_start, size_t pages, phy_addr_t phy_start, u64 flags, u64 actions);

    void big_page_map_to(void *virt_start, size_t pages, phy_addr_t phy_start, u64 flags, u64 actions);
    void huge_page_map_to(void *virt_start, size_t pages, phy_addr_t phy_start, u64 flags, u64 actions);

    void prepare_kernel_space();

    void map_kernel_space();

    void unmap(void *virt_start, size_t pages);

    bool has_flags(void *virt_start, u64 flags = flags::present);

    void clone_readonly_to(void *virt_start, size_t pages, page_table_t *to);

    freelibcxx::optional<phy_addr_t> get_map(void *virt_start);

  private:
    void ensure(int pml4e_index, int pdpe_index, int pde_index, u64 flags, u64 actions);
    void ensure(int pml4e_index, int pdpe_index, u64 flags, u64 actions);
    void ensure(int pml4e_index, u64 flags, u64 actions);

    inline bool normalize_index(int &pml4e_index, int &pdpe_index, int &pde_index, int &pte_index)
    {
        bool need_check = false;
        if (pte_index >= 512)
        {
            pte_index = 0;
            pde_index++;
            need_check = true;
        }
        if (pde_index >= 512)
        {
            pde_index = 0;
            pdpe_index++;
            need_check = true;
        }
        if (pdpe_index >= 512)
        {
            pdpe_index = 0;
            pml4e_index++;
            need_check = true;
        }
        if (pml4e_index >= 512)
        {
        }
        return need_check;
    }

    index_t from_virt_addr(void *virt_addr)
    {
        auto p = reinterpret_cast<uintptr_t>(virt_addr);
        return from_virt_addr(p);
    }
    index_t from_virt_addr(uintptr_t p) { return index_t(p >> 12); }
    uintptr_t to_virt_addr(index_t p) { return p.index << 12; }

  private:
    pml4t *base_;
    bool self_;
};

} // namespace arch::paging
