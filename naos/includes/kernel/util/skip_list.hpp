#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
#include "random.hpp"
#include <utility>

namespace util
{
template <typename E> class skip_list
{
  public:
    struct node_t;
    using list_node = node_t;
    class iterator;
    using level_t = u16;

    /// return -1 walk left
    /// 1 walk right
    /// 0 stop
    typedef int (*each_func)(const E &element, u64 user_data);

    skip_list(memory::IAllocator *allocator, u8 rand_factor = 2, level_t max_level = 32)
        : count(0)
        , level(1)
        , max_level(max_level)
        , rand_factor(rand_factor)
        , allocator(allocator)
    {
        init();
    }

    skip_list(memory::IAllocator *allocator, std::initializer_list<E> il)
        : skip_list(allocator)
    {
        for (auto e : il)
        {
            insert(std::move(e));
        }
    }

    skip_list(const skip_list &rhs) { copy(rhs); }

    skip_list(skip_list &&rhs) { move(std::move(rhs)); }

    ~skip_list() { free(); }

    skip_list &operator=(const skip_list &rhs)
    {
        if (this == &rhs)
            return *this;
        free();
        copy(rhs);
        return *this;
    }

    skip_list &operator=(skip_list &&rhs)
    {
        if (this == &rhs)
            return *this;
        free();
        move(std::move(rhs));
        return *this;
    }

    iterator insert(E &&element)
    {
        level_t node_level = this->rand();

        if (node_level > level)
            level = node_level;

        // for each level of node
        // find the element path
        auto node = list[max_level - level];
        for (level_t i = 0; i < level; i++)
        {
            while (node->next && node->next->end_node->element < element)
            {
                node = node->next;
            }
            stack[i] = node;
            node = node->child;
        }

        // create end node
        auto end_node_prev = stack[level - 1];
        auto new_end_node = memory::New<node_t>(allocator, end_node_prev->next, std::move(element));
        end_node_prev->next = new_end_node;
        count++;

        // set index node linked list
        level_t base = level - node_level;
        for (level_t i = node_level - 1; i > 0; i--)
        {
            stack[base + i - 1]->next =
                memory::New<node_t>(allocator, stack[base + i - 1]->next, stack[base + i]->next, new_end_node);
        }

        return iterator(new_end_node);
    }

    iterator remove(const E &element)
    {
        auto node = list[max_level - level];
        level_t lev_delete = 0;
        level_t cur_level = 0;
        // for each get element per level
        for (level_t i = 0; i < level; i++)
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
                cur_level++;
                if (i < level - 1 && !list[i + max_level - level]->next)
                    lev_delete++;
            }
            stack[i] = node;
            node = node->child;
        }
        if (cur_level == 0)
            return iterator(nullptr);

        if (cur_level == level)
            level -= lev_delete;

        count--;
        return iterator(stack[cur_level - 1]->next);
    }

    iterator remove(iterator iter) { return remove(*iter); }

    u64 size() const { return count; }

    bool empty() const { return count == 0; }

    u64 deep() const { return level; }

    E front() const { return list[max_level - 1]->next->element; }

    iterator begin() const { return iterator(list[max_level - 1]->next); }

    iterator end() const { return iterator(nullptr); }

    iterator find(const E &element) const
    {
        node_t *node = list[max_level - level], *last_node = nullptr;

        for (level_t i = 0; i < level; i++)
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

    bool has(const E &element) const { return find(element) != end(); }

    iterator find_before(const E &element) const
    {
        node_t *node = list[max_level - level], *last_node = nullptr;

        for (level_t i = 0; i < level; i++)
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
        node_t *node = list[max_level - level];

        for (level_t i = 0; i < level; i++)
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
        node_t *node = list[max_level - level], *last_node = nullptr;

        for (level_t i = 0; i < level; i++)
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

    void clear()
    {
        count = 0;
        for (level_t i = 0; i < level; i++)
        {
            node_t *p0 = list[max_level - i - 1];
            node_t *node = p0->next;
            while (node)
            {
                auto next = node->next;
                memory::Delete<node_t>(allocator, node);
                node = next;
            }
            p0->next = nullptr;
        }
    }

  private:
    u64 count;

    level_t level;
    level_t max_level;

    u8 rand_factor;

    node_t **stack;
    /// an array likes  [
    ///  node_level 1
    ///  node_level 2
    ///  node_level 3
    ///  node_level 4 root
    /// ]
    node_t **list;
    memory::IAllocator *allocator;

    level_t rand()
    {
        level_t node_level = 1;
        while (next_rand(rand_factor) == 0 && node_level < max_level)
            node_level++;
        return node_level;
    }

    void free()
    {
        if (stack != nullptr)
        {
            clear();
            count = 0;
            level = 0;
            rand_factor = 0;
            memory::DeleteArray<>(allocator, stack, max_level);
            for (u32 i = 0; i < max_level; i++)
            {
                memory::Delete<>(allocator, list[max_level - i - 1]);
            }
            memory::DeleteArray<>(allocator, list, max_level);
        }
    }

    void init()
    {
        stack = memory::NewArray<node_t *>(allocator, max_level);
        list = memory::NewArray<node_t *>(allocator, max_level);

        node_t *last = nullptr;
        for (level_t i = 0; i < max_level; i++)
        {
            list[max_level - i - 1] = memory::New<node_t>(allocator, nullptr, last, nullptr);
            last = list[max_level - i - 1];
        }
    }

    void copy(const skip_list &rhs)
    {
        count = 0;
        level = 1;
        max_level = rhs.max_level;
        rand_factor = rhs.rand_factor;
        allocator = rhs.allocator;
        init();
        // n*log(n)
        auto iter = rhs.begin();
        auto end = rhs.end();
        while (iter != end)
        {
            E v = *iter;
            insert(std::move(v));
            iter++;
        }
    }

    void move(skip_list &&rhs)
    {
        count = rhs.count;
        level = rhs.level;
        max_level = rhs.max_level;
        rand_factor = rhs.rand_factor;
        stack = rhs.stack;
        list = rhs.list;
        allocator = rhs.allocator;
        rhs.count = 0;
        rhs.level = 0;
        rhs.stack = nullptr;
        rhs.list = nullptr;
    }

  public:
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

        node_t(node_t *next, E &&e)
            : next(next)
            , end_node(this)
            , element(std::move(e))
        {
        }
    };

}; // namespace util
} // namespace util