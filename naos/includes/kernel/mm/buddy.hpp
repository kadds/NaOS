#pragma once
#include "common.hpp"
#include "memory.hpp"

namespace memory
{

///< For debugging
struct buddy_tree
{
    buddy_tree *left;
    buddy_tree *right;
    u64 val;
};

extern const int buddy_max_page;

class buddy
{
  private:
    const int size;
    /// The 0th to last element comes from the output of the binary tree traversal. Element 0 is root.
    u16 *array;
    u64 fit_size(u64 size);

  public:
    buddy(int max_level);
    ~buddy() = default;
    buddy(const buddy &) = delete;
    buddy &operator=(const buddy &) = delete;

    int alloc(u64 size);
    void free(int offset);

    bool tag_alloc(int start_offset, int len);
    // for debug
    buddy_tree *gen_tree();
    // also for debug
    void cat_tree(buddy_tree *tree, int index);
};

struct buddy_contanier
{
    int count;
    buddy *buddies;
};

class BuddyAllocator : public IAllocator
{
  public:
    BuddyAllocator();
    ~BuddyAllocator();
    void *allocate(u64 size, u64 align) override;
    void deallocate(void *ptr) override;
};
extern BuddyAllocator *KernelBuddyAllocatorV;

} // namespace memory
