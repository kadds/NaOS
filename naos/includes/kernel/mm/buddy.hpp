#pragma once
#include "common.hpp"
#include "memory.hpp"
// for debug
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
    // From element 0 to elment last is the reslut of binary tree traversal. Element 0 is root.
    // Each element represent from 4 bits. So max level is 2 ^ 4 = 16
    // Using impl for avoid cyclic dependence
    void *bits_impl;
    u64 fit_size(u64 size);

  public:
    int alloc(u64 size);
    void free(int offset);

    bool tag_alloc(int start_offset, int len);
    buddy(int max_level);
    // for debug
    buddy_tree *gen_tree();
    // also for debug
    void cat_tree(buddy_tree *tree, int index);
};

struct buddy_contanier
{
    int count;
    buddy *buddys;
};
namespace memory
{

class BuddyAllocator : public IAllocator
{
  private:
    // only the same flags in the zone can be assigned
    u64 flags;

  public:
    BuddyAllocator(u64 flags);
    ~BuddyAllocator();
    void *allocate(u64 size, u64 align) override;
    void deallocate(void *ptr) override;
};
} // namespace memory
