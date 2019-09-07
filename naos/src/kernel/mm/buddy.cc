#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/util/bit_set.hpp"

typedef util::bit_set<u64, 16> bits_t;

const int buddy_max_page = 1 << 8;

bits_t &get(void *impl) { return *(bits_t *)impl; }

u64 buddy::fit_size(u64 size)
{
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
    return size + 1;
}

int buddy::alloc(u64 pages)
{
    auto &bitset = get(bits_impl);
    if (pages == 0)
        pages = 1;
    else if ((pages & (pages - 1)) != 0) // not pow of 2
        pages = fit_size(pages);

    // The existing maximum size does not apply to callers.
    if (bitset.get(0) < pages)
        return -1;

    // Find block
    u64 target_page = (size + 1) / 2;
    int i = 0;
    for (; target_page != pages; target_page >>= 1)
    {
        if (bitset.get(i * 2 + 1) >= pages) // Available at left branch
        {
            i = i * 2 + 1;
        }
        else // Available at right branch
        {
            i = i * 2 + 2;
        }
    }
    bitset.set(i, 0);
    int index = i;
    while (index >= 0) // Modify the value of it
    {
        index = ((index + 1) >> 1) - 1;
        auto a = bitset.get(index * 2 + 1);
        auto b = bitset.get(index * 2 + 2);
        if (a < b) // maximum size
        {
            a = b;
        }
        bitset.set(index, a); // reset maximum size
    }
    return (i + 1) * target_page - (size + 1) / 2;
}

void buddy::free(int offset)
{
    auto &bitset = get(bits_impl);
    int index = offset + (size + 1) / 2 - 1;
    u64 node_size = 1;
    for (; bitset.get(index); index = ((index + 1) >> 1) - 1)
    {
        node_size *= 2;
        if (index == 0)
            return;
    }
    bitset.set(index, node_size);

    while (index)
    {
        index = ((index + 1) >> 1) - 1;
        node_size *= 2;

        auto left = bitset.get(index * 2 + 1);
        auto right = bitset.get(index * 2 + 2);

        if (left + right == node_size)
            bitset.set(index, node_size);
        else
            bitset.set(index, left > right ? left : right);
    }
}

buddy::buddy(int page_count)
    : size(page_count * 2 - 1)
    , bits_impl(memory::New<bits_t>(memory::VirtBootAllocatorV, memory::VirtBootAllocatorV, size))
{
    if ((page_count & (page_count - 1)) != 0)
        return;
    u64 node_size = size + 1;
    auto &bitset = get(bits_impl);
    for (int i = 0; i < size; i++)
    {
        if (((i + 1) & (i)) == 0)
        {
            node_size >>= 1;
        }

        bitset.set(i, node_size);
    }
}

/*
Tag memory used by kernel.
*/
bool buddy::tag_alloc(int start_offset, int len)
{
    auto &bitset = get(bits_impl);
    // no include this [start_offset, end_offset)
    int end_offset = start_offset + len - 1;

    int start_index = start_offset + (size + 1) / 2 - 1;
    int end_index = end_offset + (size + 1) / 2 - 1;
    for (int i = start_index; i <= end_index; i++)
    {
        bitset.set(i, 0);
    }
    int sindex = start_index, eindex = end_index;
    u64 node_size = size + 1;
    while (sindex > 0 && eindex > 0)
    {
        sindex = ((sindex + 1) >> 1) - 1;
        eindex = ((eindex + 1) >> 1) - 1;
        for (int i = sindex; i <= eindex; i++)
        {
            auto left = bitset.get(i * 2 + 1);
            auto right = bitset.get(i * 2 + 2);

            if (left + right == node_size)
                bitset.set(i, node_size); // merge
            else
            {
                auto v = left > right ? left : right;
                bitset.set(i, v);
            }
        }
        node_size >>= 1;
    }
    return true;
}

void buddy::cat_tree(buddy_tree *tree, int index)
{
    if (index > size)
    {
        return;
    }
    tree->val = get(bits_impl).get(index);

    if (index * 2 + 2 > size)
    {
        tree->left = nullptr;
        tree->right = nullptr;
    }
    else
    {

        tree->left = tree + index * 2 + 1 - index;
        tree->right = tree + index * 2 + 2 - index;
        cat_tree(tree->left, index * 2 + 1);
        cat_tree(tree->right, index * 2 + 2);
    }
}
buddy_tree *buddy::gen_tree()
{
    buddy_tree *tree = memory::NewArray<buddy_tree>(memory::VirtBootAllocatorV, size);
    cat_tree(tree, 0);
    return tree;
}

// buddy allocator
namespace memory
{

BuddyAllocator::BuddyAllocator(u64 flags)
    : flags(flags)
{
}

BuddyAllocator::~BuddyAllocator() {}

void *BuddyAllocator::allocate(u64 size, u64 align)
{
    auto page = (size + 0x1000 - 1) / 0x1000;
    for (int i = 0; i < global_zones.count; i++)
    {
        zone_t &zone = global_zones.zones[i];
        if ((zone.props & flags) != flags)
            continue;
        auto buddys = (buddy_contanier *)zone.buddy_impl;
        for (int j = 0; j < buddys->count; j++)
        {
            i64 offset = buddys->buddys[j].alloc(page);
            if (offset >= 0)
            {
                auto ptr = (char *)zone.start + offset * memory::page_size;
                return memory::kernel_phyaddr_to_virtaddr((char *)ptr + buddy_max_page * page_size * j);
            }
        }
    }
    return nullptr;
}

void BuddyAllocator::deallocate(void *ptr)
{
    ptr = memory::kernel_virtaddr_to_phyaddr(ptr);
    for (int i = 0; i < global_zones.count; i++)
    {
        zone_t &zone = global_zones.zones[i];
        if ((zone.props & flags) != flags)
            continue;
        if ((char *)ptr >= zone.start && (char *)ptr < zone.end)
        {
            auto buddys = (buddy_contanier *)zone.buddy_impl;

            int buddy_index = (u64)((char *)ptr - (char *)zone.start + (memory::page_size * buddy_max_page) - 1) /
                                  (memory::page_size * buddy_max_page) -
                              1;
            i64 offset =
                ((char *)ptr - buddy_max_page * page_size * buddy_index - (char *)zone.start) / memory::page_size;
            kassert(offset >= 0 && offset <= buddy_max_page,
                    "offset should not less than 0 or more than buddy max page");

            buddys->buddys[buddy_index].free(offset);
            break;
        }
    }
}
} // namespace memory
