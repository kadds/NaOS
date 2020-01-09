#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
#include "random.hpp"
namespace util
{
template <typename E> class skip_list
{
  public:
    struct end_node_t
    {
        end_node_t *next;
        end_node_t *end_node;
        E element;
        end_node_t(const E &element, end_node_t *next)
            : next(next)
            , end_node(this)
            , element(element)
        {
        }
    };

    struct inode_t
    {
        inode_t *next;
        end_node_t *end_node;
        inode_t *child;
        inode_t(end_node_t *end_node, inode_t *next, inode_t *child)
            : next(next)
            , end_node(end_node)
            , child(child)

        {
        }

        inode_t(inode_t *next, inode_t *child)
            : next(next)
            , child(child)
        {
        }
    };

    struct node_t
    {
        union
        {
            end_node_t end_node;
            inode_t inode;
        };
    };

    using list_node = node_t;

  private:
    memory::IAllocator *allocator;
    inode_t *root_list;
    u64 height, count;

    /// note: part is scale to 10000
    u32 part;
    u32 max_height;

    inode_t **stack;

    end_node_t *tail_list;

    u32 rand()
    {
        u32 node_height = 1;
        while (next_rand(part) == 0 && node_height <= max_height)
            node_height++;
        return node_height;
    }

  public:
    class iterator
    {
        end_node_t *node;

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

        iterator(end_node_t *node)
            : node(node)
        {
        }
    };

    iterator insert(const E &element)
    {
        u32 node_height = rand();

        // expand height
        u64 h = 0;
        if (height < node_height)
        {
            h = node_height - height;
            // link root inode
            stack[h] = root_list;
            for (u64 i = h; i >= 1; i--)
            {
                stack[h - 1] = memory::New<inode_t>(allocator, nullptr, nullptr, stack[h]);
            }
            root_list = stack[0];
        }
        else
        {
            stack[h] = root_list;
        }

        // for each index node
        inode_t *node = stack[h];
        for (u64 i = 0; i < height; i++)
        {
            while (node->next && node->next->end_node->element < element)
            {
                node = node->next;
            }
            stack[h++] = node;
            node = node->child;
        }

        // create end node
        auto end_node_prev = (end_node_t *)stack[h - 1];
        auto new_node = memory::New<end_node_t>(allocator, element, end_node_prev->next);
        end_node_prev->next = new_node;
        count++;

        if (height < node_height)
        {
            height = node_height;
        }
        // set index node linked list
        node = (inode_t *)new_node;
        for (u64 i = 0; i < node_height - 1; i++)
        {
            auto inode = memory::New<inode_t>(allocator, new_node, stack[height - i - 2]->next, node);
            stack[height - i - 2]->next = inode;
            node = inode;
        }

        return iterator(new_node);
    }

    iterator remove(const E &element)
    {
        auto node = root_list;
        u64 h = 0;
        // for each get element per level
        for (u64 i = 0; i < height; i++)
        {
            while (node->next && node->next->end_node->element < element)
            {
                node = node->next;
            }
            stack[h++] = node;
            node = node->child;
        }

        // remove inodes
        // tag if next is element to delete
        bool eq = false;
        for (u64 i = 0; i < h - 1; i++)
        {
            if (eq || (stack[i]->next && stack[i]->next->end_node->element == element))
            {
                node = stack[i]->next;
                stack[i]->next = node->next;
                memory::Delete<>(allocator, node);
                eq = true;
            }
        }

        count--;

        node = root_list;
        // try remove root list
        for (u64 i = 0; i < h - 1; i++)
        {
            if (!node->next)
            {
                auto child = node->child;
                memory::Delete<>(allocator, node);
                node = child;
                height--;
                root_list = node;
                continue;
            }
            break;
        }

        // remove tail node

        auto last_end_node = (end_node_t *)stack[h - 1];
        if (last_end_node->next)
        {
            auto next_node = last_end_node->next;
            last_end_node->next = (end_node_t *)next_node->next;
            memory::Delete<>(allocator, next_node);
            return iterator(last_end_node->next);
        }
        return iterator(nullptr);
    }

    iterator remove(iterator iter) { return remove(*iter); }

    u64 size() { return count; }

    bool empty() { return count == 0; }

    u64 deep() { return height; }

    E front() { return tail_list->next->element; }

    iterator begin() { return iterator(tail_list->next); }

    iterator end() { return iterator(nullptr); }

    iterator find(const E &element) const
    {
        inode_t *node = root_list, *last_node = nullptr;

        for (u64 i = 0; i < height; i++)
        {
            while (node->next && node->next->end_node->element < element)
            {
                node = node->next;
            }
            last_node = node;
            node = node->child;
        }
        return iterator((end_node_t *)last_node);
    }

    skip_list(memory::IAllocator *allocator, u32 part = 2, u32 max_height = 24)
        : allocator(allocator)
        , height(1)
        , count(0)
        , part(part)
        , max_height(max_height)
    {
        stack = (inode_t **)memory::KernelCommonAllocatorV->allocate(sizeof(inode_t *) * max_height, 8);
        root_list = memory::New<inode_t>(allocator, nullptr, nullptr, nullptr);
        tail_list = (end_node_t *)root_list;
    }

    ~skip_list() { memory::KernelCommonAllocatorV->deallocate(stack); }

    skip_list(const skip_list &list) {}

    skip_list *operator=(const skip_list &list) { return *this; }
}; // namespace util
} // namespace util