#pragma once
#include "../mm/new.hpp"
#include "assert.hpp"
#include "common.hpp"
#include "iterator.hpp"
#include <utility>

namespace util
{
template <typename E> class linked_list
{
  public:
    struct list_node;

  private:
    template<typename N, typename K>
    struct value_fn
    {
        K operator()(N val) { return &val->element; }
    };
    template<typename N>
    struct prev_fn
    {
        N operator()(N val) { return val->prev; }
    };
    template<typename N>
    struct next_fn
    {
        N operator()(N val) { return val->next; }
    };
    using NE = list_node *;
    using CE = const list_node *;

  public:
    
    using const_iterator = base_bidirectional_iterator<CE, value_fn<CE, const E*>, prev_fn<CE>, next_fn<CE>>;
    using iterator = base_bidirectional_iterator<NE, value_fn<NE, E *>, prev_fn<NE>, next_fn<NE>>;

    struct list_info_node;

    linked_list(memory::IAllocator *allocator)
        : allocator(allocator)
        , head(memory::New<list_info_node>(allocator))
        , tail(memory::New<list_info_node>(allocator))
        , node_count(0)
    {
        head->next = (list_node *)tail;
        head->prev = nullptr;
        tail->prev = (list_node *)head;
        tail->next = nullptr;
    };

    linked_list(memory::IAllocator *allocator, std::initializer_list<E> il)
        : linked_list(allocator)
    {
        for (const E &a : il)
        {
            push_back(a);
        }
    };

    ~linked_list()
    {
        free();
    }

    linked_list(const linked_list &rhs) {
        copy(rhs);
    }

    linked_list(linked_list &&rhs) {
        move(std::move(rhs));
    }

    linked_list &operator=(const linked_list &rhs)
    {
        if (unlikely(&rhs == this))
            return *this;
        free();
        copy(rhs);
        return *this;
    }

    linked_list &operator=(linked_list &&rhs) {
        if (unlikely(&rhs == this)) 
            return *this;
        free();
        move(std::move(rhs));
        return *this;
    }

    template<typename ...Args>
    iterator push_back(Args && ...args)
    {
        list_node *node = memory::New<list_node>(allocator, std::forward<Args>(args)...);
        node->next = (list_node *)tail;
        tail->prev->next = node;
        node->prev = tail->prev;
        tail->prev = node;
        node_count++;
        return iterator(node);
    }

    template<typename ...Args>
    iterator push_front(Args && ...args)
    {
        list_node *node = memory::New<list_node>(allocator, std::forward<Args>(args)...);
        list_node *next = ((list_node *)head)->next;
        ((list_node *)head)->next = node;
        node->next = next;
        node->prev = ((list_node *)head);
        next->prev = node;
        node_count++;
        return iterator(node);
    }

    E pop_back()
    {
        cassert(tail->prev != (list_node*)head && node_count > 0);
        E e = tail->prev->element;
        list_node *node = tail->prev;
        node->prev->next = (list_node *)tail;
        tail->prev = node->prev;
        memory::Delete<>(allocator, node);
        node_count--;
        return e;
    };

    E pop_front()
    {
        cassert(tail->prev != (list_node*)head && node_count > 0);
        E e = head->next->element;
        list_node *node = head->next;
        node->next->prev = (list_node *)head;
        tail->next = node->prev;
        head->next = head->next->next;
        memory::Delete<>(allocator, node);
        node_count--;
        return e;
    };

    u64 size() const { return node_count; }

    iterator begin() const {
        return iterator(head->next); 
    }

    iterator end() const { return iterator((list_node *)tail); }

    iterator rbegin() const { return iterator(((list_node *)tail)->prev); }

    iterator rend() const { return iterator((list_node *)head); }

    bool empty() const { return node_count == 0; }

    const E &back() const { 
        cassert(tail->prev != (list_node*)head && node_count > 0);
        return tail->prev->element; 
    }

    const E &front() const { 
        cassert(tail->prev != (list_node*)head && node_count > 0);
        return head->next->element; 
    }

    E &back() { 
        cassert(tail->prev != (list_node*)head && node_count > 0);
        return tail->prev->element; 
        }

    E &front() { 
        cassert(tail->prev != (list_node*)head && node_count > 0);
        return head->next->element; 
    }

    /// return node if element finded else return end()
    iterator find(const E &e)
    {
        auto it = begin();
        auto last = end();
        while (it != last)
        {
            if (*it == e)
            {
                return it;
            }
            ++it;
        }
        return last;
    }

    /// insert node before parameter iter
    template <typename ...Args>
    iterator insert(iterator iter, Args ... args)
    {
        list_node *after_node = iter.get();
        list_node *node = memory::New<list_node>(allocator, std::forward<Args>(args)...);
        auto last = after_node->prev;

        last->next = node;
        node->next = after_node;
        node->prev = last;
        after_node->prev = node;
        node_count++;
        return iterator(node);
    }

    iterator remove(iterator iter)
    {
        if (unlikely(iter == end() || iter == --begin()))
            return end();

        list_node *node = iter.get();
        list_node *next = node->next;
        node->prev->next = node->next;
        node->next->prev = node->prev;
        node_count--;
        memory::Delete<>(allocator, node);
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
        tail->prev = (list_node *)head;
        node_count = 0;
    }

    E &at(u64 idx)
    {
        cassert(idx < node_count);
        u64 i = 0;
        auto it = begin();
        auto last = end();
        while (it != last && i < idx)
        {
            ++it;
            i++;
        }
        return *it;
    }

  private:
    void free()
    {
        if (head != nullptr) {
            clear();
            memory::Delete<>(allocator, head);
            memory::Delete<>(allocator, tail);
            head = nullptr;
            tail = nullptr;
        }
    }

    void copy(const linked_list &rhs) {
        node_count = 0;
        allocator = rhs.allocator;
        head = memory::New<list_info_node>(allocator);
        tail = memory::New<list_info_node>(allocator);

        head->next = (list_node *)tail;
        head->prev = nullptr;
        tail->prev = (list_node *)head;
        tail->next = nullptr;
        auto iter = rhs.begin();
        while (iter != rhs.end())
        {
            E val = *iter;
            push_back(std::move(val));
            iter++;
        }
    }

    void move(linked_list && rhs) {
        allocator = rhs.allocator;
        head = rhs.head;
        tail = rhs.tail;
        node_count = rhs.node_count;
        rhs.head = nullptr;
        rhs.tail = nullptr;
        rhs.node_count = 0;
    }

  private:
    memory::IAllocator *allocator;
    list_info_node *head, *tail;
    u64 node_count;

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

  public:
    struct list_node
    {
        E element;
        list_node *prev, *next;

        template <typename... Args>
        list_node(Args &&...args)
            : element(std::forward<Args>(args)...)
        {
        }
    };
    struct list_info_node
    {
        char data[sizeof(list_node) - sizeof(list_node *) * 2];
        list_node *prev, *next;
        list_info_node(){};
    };

    static_assert(sizeof(list_node) == sizeof(list_info_node));
   
};
} // namespace util