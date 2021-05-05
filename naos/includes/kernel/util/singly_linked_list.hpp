#pragma once
#include "../mm/new.hpp"
#include "assert.hpp"
#include "common.hpp"
#include "iterator.hpp"
#include <utility>

namespace util
{
template <typename E> class singly_linked_list
{
  public:
    struct list_node;
    struct list_info_node;

  private:
    template <typename N, typename K> struct value_fn
    {
        K operator()(N val) { return &val->element; }
    };
    template <typename N> struct next_fn
    {
        N operator()(N val) { return val->next; }
    };

    using NE = list_node *;
    using CE = const list_node *;

  public:
    using const_iterator = base_forward_iterator<CE, value_fn<CE, const E *>, next_fn<CE>>;
    using iterator = base_forward_iterator<NE, value_fn<NE, E *>, next_fn<NE>>;

    singly_linked_list(memory::IAllocator *allocator)
        : head(memory::New<list_info_node>(allocator))
        , tail(memory::New<list_info_node>(allocator))
        , node_count(0)
        , allocator(allocator)
    {
        head->next = (list_node *)tail;
        tail->next = nullptr;
    };

    singly_linked_list(memory::IAllocator *allocator, std::initializer_list<E> il)
        : singly_linked_list(allocator)
    {
        auto iter = il.begin();
        auto end = il.end();
        if (iter != end)
        {
            E val = *iter;
            push_front(std::move(val));
            iter++;
        }
        auto last_insert_iter = begin();
        while (iter != end)
        {
            E val = *iter;
            last_insert_iter = insert_after(last_insert_iter, std::move(val));
            iter++;
        }
    };

    ~singly_linked_list()
    {
        free();
    }

    singly_linked_list(const singly_linked_list &rhs)
    {
        copy(rhs);
    }

    singly_linked_list(singly_linked_list &&rhs)
    {
        move(std::move(rhs));
    }

    singly_linked_list &operator=(const singly_linked_list &rhs)
    {
        if (unlikely(&rhs == this))
            return *this;
        
        free();
        copy(rhs);
        return *this;
    }

    singly_linked_list &operator=(singly_linked_list &&rhs) {

        if (unlikely(&rhs == this))
            return *this;
        
        free();
        move(std::move(rhs));
        return *this;
    }

    template <typename... Args> iterator push_front(Args &&...args)
    {
        list_node *node = memory::New<list_node>(allocator, std::forward<Args>(args)...);
        node->next = head->next;
        head->next = node;
        node_count++;
        return iterator(node);
    }

    E pop_front()
    {
        cassert(head->next != tail && node_count > 0);

        E e = head->next->element;
        list_node *node = head->next;
        head->next = head->next->next;
        memory::Delete<>(allocator, node);
        node_count--;
        return e;
    };

    u64 size() const { return node_count; }

    iterator begin() const {
        return iterator(head->next); 
    }

    iterator end() const { 
        return iterator((list_node *)tail); 
    }

    bool empty() const { return head->next == (list_node *)tail; }

    const E& front() const { cassert(node_count > 0); return head->next->element; }

    E& front() { cassert(node_count > 0); return head->next->element; }

    template <typename... Args> iterator insert_after(iterator iter, Args &&...args)
    {
        list_node *prev_node = iter.get();
        list_node *node = memory::New<list_node>(allocator, std::forward<Args>(args)...);

        node->next = prev_node->next;
        prev_node->next = node;

        node_count++;
        return iterator(node);
    }

    iterator remove(iterator iter)
    {
        if (unlikely(iter == end() || iter == iterator((list_node *)head)))
            return end();
        list_node *pnode;
        if (iter == begin())
        {
            pnode = (list_node *)head;
        }
        else
        {
            iterator prev = previous_iterator(begin(), iter);
            pnode = prev.get();
        }

        auto inode = iter.get();
        auto next = iter.get()->next;
        pnode->next = next;
        memory::Delete<>(allocator, inode);
        node_count--;
        return iterator(next);
    }

    void clear()
    {
        auto node = ((list_node *)head)->next;
        while (node != (list_node *)tail)
        {
            auto node2 = node->next;
            memory::Delete<>(allocator, node);
            node = node2;
        }
        head->next = (list_node *)tail;
        node_count = 0;
    }

    E &at(u64 idx)
    {
        u64 i = 0;
        iterator it = begin();
        iterator last = end();
        while (it != last && i < idx)
        {
            ++it;
            i++;
        }
        return *it;
    }

  private:
    list_info_node *head, *tail;
    u64 node_count;
    memory::IAllocator *allocator;

    u64 calc_size() const
    {
        u64 i = 0;
        auto iter = begin();
        while (iter != end())
        {
            i++;
            ++iter;
        }
        return i;
    }

    void free()
    {
        if (head != nullptr)
        {
            clear();
            memory::Delete<>(allocator, head);
            memory::Delete<>(allocator, tail);
            head = nullptr;
            tail = nullptr;
            node_count = 0;
        }
    }

    void copy(const singly_linked_list &rhs)
    {
        node_count = 0;
        allocator = rhs.allocator;
        head = memory::New<list_info_node>(allocator);
        tail = memory::New<list_info_node>(allocator);

        head->next = (list_node *)tail;
        tail->next = nullptr;

        auto iter = rhs.begin();
        auto end = rhs.end();
        if (iter != end)
        {
            E val = *iter;
            push_front(std::move(val));
            iter++;
        }
        auto last_insert_iter = begin();
        while (iter != end)
        {
            E val = *iter;
            last_insert_iter = insert_after(last_insert_iter, std::move(val));
            iter++;
        }
    }

    void move(singly_linked_list &&rhs)
    {
        allocator = rhs.allocator;
        head = rhs.head;
        tail = rhs.tail;
        node_count = rhs.node_count;
        rhs.head = nullptr;
        rhs.tail = nullptr;
        rhs.node_count = 0;
    }

  public:
    struct list_node
    {
        E element;
        list_node *next;

        template <typename... Args>
        list_node(Args &&...args)
            : element(std::forward<Args>(args)...)
        {
        }
    };
    struct list_info_node
    {
        char data[sizeof(list_node) - sizeof(list_node *)];
        list_node *next;
        list_info_node(){};
    };
    static_assert(sizeof(list_node) == sizeof(list_info_node));
};
} // namespace util