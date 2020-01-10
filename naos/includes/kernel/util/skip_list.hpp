#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
#include "random.hpp"
namespace util
{
template <typename E> class skip_list
{
  public:
    struct node_t
    {
        node_t *next;
        node_t *end_node;
        union
        {
            node_t *child;
            E element;
        };

        node_t(node_t *next, node_t *child, node_t *end_node)
            : next(next)
            , end_node(end_node)
            , child(child)
        {
        }

        node_t(node_t *next, const E &e)
            : next(next)
            , end_node(this)
            , element(e)
        {
        }
    };

    using list_node = node_t;

  private:
    memory::IAllocator *allocator;

    u64 count;

    u32 lev;
    u32 max_lev;

    /// note: part is scale to 10000
    u32 part;

    node_t **stack;
    node_t **list;

    u32 rand()
    {
        u32 node_lev = 1;
        while (next_rand(part) == 0 && node_lev <= max_lev)
            node_lev++;
        return node_lev;
    }

  public:
    /// return -1 walk left
    /// 1 walk right
    /// 0 stop
    typedef int (*each_func)(const E &element, u64 user_data);

    class iterator
    {
        node_t *node;

      public:
        E &operator*() { return node->element; }
        E *operator->() { return &node->element; }
        E *operator&() { return &node->element; }

        iterator operator++(int)
        {
            auto old = *this;
            node = node->next;
            return old;
        }

        iterator &operator++()
        {
            node = node->next;
            return *this;
        }

        bool operator==(const iterator &it) { return it.node == node; }

        bool operator!=(const iterator &it) { return it.node != node; }

        iterator(node_t *node)
            : node(node)
        {
        }
    };

    iterator insert(const E &element)
    {
        u32 node_lev = rand();

        if (node_lev > lev)
            lev = node_lev;

        // for each level node
        auto node = list[max_lev - lev];
        for (u32 i = 0; i < lev; i++)
        {
            while (node->next && node->next->end_node->element < element)
            {
                node = node->next;
            }
            stack[i] = node;
            node = node->child;
        }

        // create end node
        auto end_node_prev = stack[lev - 1];
        auto new_end_node = memory::New<node_t>(allocator, end_node_prev->next, element);
        end_node_prev->next = new_end_node;
        count++;

        // set index node linked list
        u32 base = lev - node_lev;
        for (u32 i = node_lev - 1; i > 0; i--)
        {
            stack[base + i - 1]->next =
                memory::New<node_t>(allocator, stack[base + i - 1]->next, stack[base + i]->next, new_end_node);
        }

        return iterator(new_end_node);
    }

    iterator remove(const E &element)
    {
        auto node = list[max_lev - lev];
        u32 lev_del = 0;
        u32 cur_lev = 0;
        // for each get element per level
        for (u32 i = 0; i < lev; i++)
        {
            while (node->next && node->next->end_node->element < element)
            {
                node = node->next;
            }
            if (node->next && node->next->end_node->element == element)
            {
                auto next = node->next->next;
                memory::Delete<>(allocator, node->next);
                node->next = next;
                cur_lev++;
                if (i < lev - 1 && !list[i + max_lev - lev]->next)
                    lev_del++;
            }
            stack[i] = node;
            node = node->child;
        }
        if (cur_lev == 0)
            return iterator(nullptr);

        if (cur_lev == lev)
            lev -= lev_del;

        count--;
        return iterator(stack[cur_lev - 1]->next);
    }

    iterator remove(iterator iter) { return remove(*iter); }

    u64 size() { return count; }

    bool empty() { return count == 0; }

    u64 deep() { return lev; }

    E front() { return list[max_lev - 1]->next->element; }

    iterator begin() { return iterator(list[max_lev - 1]->next); }

    iterator end() { return iterator(nullptr); }

    iterator find(const E &element) const
    {
        node_t *node = list[max_lev - lev], *last_node = nullptr;

        for (u32 i = 0; i < lev; i++)
        {
            while (node->next && node->next->end_node->element < element)
            {
                node = node->next;
            }
            last_node = node;
            node = node->child;
        }
        if (last_node->next && last_node->next->end_node->element == element)
            return iterator(last_node->next);
        return iterator(nullptr);
    }

    iterator find_before(const E &element) const
    {
        node_t *node = list[max_lev - lev], *last_node = nullptr;

        for (u32 i = 0; i < lev; i++)
        {
            while (node->next && node->next->end_node->element < element)
            {
                node = node->next;
            }
            last_node = node;
            node = node->child;
        }
        return iterator(last_node);
    }

    iterator for_each(each_func func, u64 user_data)
    {
        node_t *node = list[max_lev - lev];

        for (u32 i = 0; i < lev; i++)
        {
            while (node->next)
            {
                int v = func(node->next->end_node->element, user_data);
                if (v == 1) // ->
                    node = node->next;
                else if (v == 0) // == eq
                    return iterator(node->next->end_node);
                else
                    break;
            }
            node = node->child;
        }
        return iterator(nullptr);
    }

    iterator for_each_last(each_func func, u64 user_data)
    {
        node_t *node = list[max_lev - lev], *last_node = nullptr;

        for (u32 i = 0; i < lev; i++)
        {
            while (node->next)
            {
                int v = func(node->next->end_node->element, user_data);
                if (v == 1) // ->
                    node = node->next;
                else if (v == 0)
                    return iterator(node->next->end_node);
                else
                    break;
            }
            last_node = node;
            node = node->child;
        }
        return iterator(last_node);
    }

    skip_list(memory::IAllocator *allocator, u32 part = 2, u32 max_lev = 16)
        : allocator(allocator)
        , count(0)
        , lev(1)
        , max_lev(max_lev)
        , part(part)
    {
        stack = (node_t **)memory::KernelCommonAllocatorV->allocate(sizeof(node_t *) * max_lev, 8);
        list = (node_t **)memory::KernelCommonAllocatorV->allocate(sizeof(node_t *) * max_lev, 8);
        list[max_lev - 1] = memory::New<node_t>(allocator, nullptr, nullptr, nullptr);
        for (u32 i = 1; i < max_lev; i++)
        {
            list[max_lev - i - 1] = memory::New<node_t>(allocator, nullptr, list[max_lev - i], nullptr);
        }
    }

    ~skip_list() { memory::KernelCommonAllocatorV->deallocate(stack); }

    skip_list(const skip_list &list) {}

    skip_list *operator=(const skip_list &list) { return *this; }
}; // namespace util
} // namespace util