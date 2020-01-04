#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
#include "random.hpp"
namespace util
{
template <typename E, typename less_cmp> class skip_list
{
    static inline constexpr u64 maximum_deep = 24;

  public:
    struct node_t
    {
        union
        {
            E element;
        };
        node_t *next;
        node_t *child;
        node_t(E element, node_t *next, node_t *child)
            : element(element)
            , next(next)
            , child(child)
        {
        }

        node_t(node_t *next, node_t *child)
            : element(0)
            , next(next)
            , child(child)
        {
        }
    };
    using list_node = node_t;

  private:
    memory::IAllocator *allocator;
    node_t *root_list;
    u64 length, count, part;
    node_t *last_list;

    void check_list()
    {
        if (count == 0 && length != 1)
        {
            count++;
            count--;
        }
    }

  public:
    class iterator
    {
        node_t *node;

      public:
        E &operator*() { return node->element; }
        E *operator->() { return &node->element; }
        E *operator&() { return &node->element; }

        iterator operator++(int) const { return iterator(node->next); }

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

    void insert(const E &element)
    {
        less_cmp cmp;
        u64 deep = 1;
        while (next_rand(part) == 0 && deep <= maximum_deep)
            deep++;

        node_t *where[maximum_deep + 1];

        // expand
        while (length < deep)
        {
            auto node = memory::New<node_t>(allocator, nullptr, root_list);
            root_list = node;
            length++;
        }

        node_t *node = root_list;
        bool root = true;
        u64 cur_deep = 0;
        while (node != nullptr)
        {
            if (unlikely(!root && node->element == element))
            {
                while (node)
                {
                    auto next = node->child;
                    where[cur_deep++] = node;
                    node = next;
                }
                break;
            }
            if (node->next)
            {
                if (cmp(node->next->element, element)) // node->next->element < cur_element
                {
                    node = node->next;
                    root = false;
                    continue;
                }
                else if (node->child == nullptr)
                {
                    /// last_deep
                    where[cur_deep++] = node;
                    break;
                }
            }
            where[cur_deep++] = node;
            node = node->child;
        }
        count++;
        node_t *parent = nullptr;
        for (u64 i = cur_deep - deep; i < cur_deep; i++)
        {
            node = memory::New<node_t>(allocator, element, where[i]->next, nullptr);
            where[i]->next = node;
            if (parent != nullptr)
                parent->child = node;
            parent = node;
        }
    }

    void remove(const E &element)
    {
        less_cmp cmp;
        auto node = root_list;
        node_t *where[maximum_deep + 1];
        u64 cur_deep = 0;

        while (node != nullptr)
        {
            if (node->next)
            {
                if (node->next->element == element) // node->next->element >= cur_element
                {
                    where[cur_deep++] = node;
                    node = node->child;
                    continue;
                }
                else if (!cmp(node->next->element, element))
                {
                    node = node->child;
                    continue;
                }
            }
            node = node->next;
        }

        for (u64 i = 0; i < cur_deep; i++)
        {
            auto n = where[i]->next;
            where[i]->next = n->next;
            memory::Delete<>(allocator, n);
        }
        if (length == cur_deep)
        {
            node = root_list;
            u64 c = length - 1;
            for (u64 i = 0; i < c; i++)
            {
                auto child = node->child;
                if (!node->next)
                {
                    memory::Delete<>(allocator, node);
                    node = child;
                    length--;
                }
                else
                    break;
            }
            root_list = node;
        }
        count--;
        check_list();
    }

    void remove(iterator iter) { remove(*iter); }

    u64 size() { return count; }

    bool empty() { return count == 0; }

    u64 deep() { return length; }

    E front() { return last_list->next->element; }

    E back()
    {
        auto n = root_list->next;
        node_t *last = root_list->next;
        while (n)
        {
            while (n->next)
            {
                last = n;
                n = n->next;
            }
            n = n->child;
        }

        return last->element;
    }

    iterator begin() { return iterator(last_list->next); }

    iterator end() { return iterator(nullptr); }

    iterator find(const E &element) const
    {
        less_cmp cmp;
        auto node = root_list;
        bool is_root = true;

        while (node != nullptr)
        {
            if (!is_root)
            {
                if (node->element == element)
                {
                    while (node->child)
                        node = node->child;
                    return iterator(node);
                }
            }
            if (node->next)
            {
                if (cmp(node->next->element, element) ||
                    node->next->element == element) // node->next->element < cur_element
                {
                    node = node->next;
                    is_root = false;
                    continue;
                }
            }
            node = node->child;
        }

        return iterator(nullptr);
    }

    iterator find_top(const E &element) const
    {
        less_cmp cmp;
        auto node = root_list;
        bool is_root = true;

        while (node != nullptr)
        {
            if (!is_root)
            {
                if (node->element == element)
                {
                    return iterator(node);
                }
            }
            if (node->next)
            {
                if (cmp(node->next->element, element)) // node->next->element < cur_element
                {
                    node = node->next;
                    is_root = false;
                    continue;
                }
            }
            node = node->child;
        }
        return iterator(nullptr);
    }

    skip_list(memory::IAllocator *allocator, u64 part = 2)
        : allocator(allocator)
        , length(1)
        , count(0)
        , part(part)
    {
        last_list = memory::New<node_t>(allocator, nullptr, nullptr);
        root_list = last_list;
    }

    skip_list(const skip_list &list) {}

    skip_list *operator=(const skip_list &list) { return *this; }
}; // namespace util
} // namespace util