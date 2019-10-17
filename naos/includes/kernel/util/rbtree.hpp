#pragma once
#include "../mm/allocator.hpp"
namespace util
{
// red black tree
template <typename T> class rbtree
{
    using Element = typename T;
    class node
    {
        node *left;
        node *right;
        Element v;

      public:
        bool is_black() const { return left & 0x1; }
        void set_black() { left = left & 1; }
        void set_red() { left = left & ~1; }
        node *get_left() { return left & ~1; }
        node *get_right() { return right; }
        void set_left(node *n) { left = n & (left & 1); }
        void set_right(node *n) { right = n; }
    };

  private:
    node *root;
    memory::IAllocator *allocator;
    void fix_up(node *n) {}

  public:
    node *find(const Element &e)
    {
        if (root != nullptr)
        {
            node *n = root;
            while (true)
            {
                if (n->v == e)
                {
                    return n;
                }
                else if (n->v < e)
                {
                    if (n->get_right() == nullptr)
                        break;
                    n = n->get_right();
                }
                else if (n->v > e)
                {
                    if (n->get_left() == nullptr)
                        break;
                    n = n->get_left();
                }
            }
            return n;
        }
        return nullptr;
    }
    void insert(const Element &e)
    {
        node *n = find(e);
        node *cur_node = memory::New<node>(allocator, e);

        if (node == nullptr)
        {
            root = cur_node;
            root->set_black();
        }
        else
        {
            cur_node->set_red();
            if (n->v > e)
            {
                n->set_left(cur_node);
            }
            else
            {
                n->set_right(cur_node);
            }
        }
        fix_up(cur_node);
    }
    void remove(node *n) {}
};
} // namespace util
