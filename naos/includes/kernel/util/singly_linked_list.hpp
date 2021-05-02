#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
#include <utility>
#include "assert.hpp"

namespace util
{
template <typename E> class singly_linked_list
{
  public:
    struct iterator;
    struct list_node;
    struct list_info_node;

    singly_linked_list(memory::IAllocator *allocator)
        : head(memory::New<list_info_node>(allocator))
        , tail(memory::New<list_info_node>(allocator))
        , node_count(0)
        , allocator(allocator)
    {
        head->next = (list_node *)tail;
        tail->next = nullptr;
        back_node = (list_node *)head;
    };

    singly_linked_list(memory::IAllocator *allocator, std::initializer_list<E> il)
        : singly_linked_list(allocator)
    {
        u64 s = il.size();
        for (E a : il)
        {
            push_back(std::move(a));
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

    iterator push_back(E &&e)
    {
        list_node *node = memory::New<list_node>(allocator, std::move(e));
        node->next = (list_node *)tail;
        back_node->next = node;
        node_count++;
        back_node = node;
        return iterator(node);
    }

    E pop_back()
    {
        cassert(head->next != tail && node_count > 0);

        auto iter = before_iterator(iterator((list_node *)head), iterator(back_node));
        iter->next = tail;
        E e = back_node->element;
        memory::Delete<list_node>(allocator, back_node);
        back_node = iter.node;
        node_count--;
        return e;
    }

    iterator push_front(E &&e)
    {
        list_node *node = memory::New<list_node>(allocator, std::move(e));
        node->next = head->next;
        head->next = node;
        node_count++;
        if (unlikely(back_node == (list_node *)head))
        {
            back_node = node;
        }
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
        if (unlikely(node_count == 0))
        {
            back_node = (list_node *)head;
        }
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

    const E& back() const { cassert(node_count > 0); return back_node->element; }

    const E& front() const { cassert(node_count > 0); return head->next->element; }

    E& back() { cassert(node_count > 0); return back_node->element; }

    E& front() { cassert(node_count > 0); return head->next->element; }

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
    iterator insert(iterator iter, const E &e) { return insert_after(before_iterator((list_node *)head, iter), e); }

    iterator insert_after(iterator iter, const E &e)
    {
        list_node *prev_node = iter.node;
        list_node *node = memory::New<list_node>(allocator, e);

        node->next = prev_node->next;
        prev_node->next = node;

        if (iter.node->next == (list_node *)tail)
        {
            back_node = iter.node->next;
        }

        node_count++;
        return iterator(node);
    }

    iterator remove(iterator iter)
    {
        if (unlikely(iter == end() || iter == iterator((list_node *)head)))
            return end();
        if (iter.node->next != (list_node *)tail)
        {
            auto del = iter.node->next;
            iter.node->next = iter.node->next->next;
            iter.node->element = del->element;
            memory::Delete<>(allocator, del);
            node_count--;
            return iterator(iter.node->next);
        }
        else
        {
            auto prev_iter = before_iterator(iterator((list_node *)head), iter);
            prev_iter.node->next = (list_node *)tail;
            memory::Delete<>(allocator, iter.node);
            back_node = prev_iter.node;
            node_count--;
            return iterator(back_node->next);
        }
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
        back_node = (list_node *)head;
        node_count = 0;
    }

    E &at(u64 idx)
    {
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
    list_info_node *head, *tail;
    list_node *back_node;
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

    iterator before_iterator(iterator start, iterator target)
    {
        auto n = start.node;
        do
        {
            if (n->next == target.node)
                break;
            n = n->next;
        } while (true);
        return iterator(n);
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
            back_node = nullptr;
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
        back_node = (list_node *)head;
        tail->next = nullptr;

        auto iter = rhs.begin();
        while (iter != rhs.end())
        {
            E val = *iter;
            push_back(std::move(val));
            iter++;
        }
    }

    void move(singly_linked_list &&rhs)
    {
        allocator = rhs.allocator;
        head = rhs.head;
        tail = rhs.tail;
        back_node = rhs.back_node;
        node_count = rhs.node_count;
        rhs.head = nullptr;
        rhs.tail = nullptr;
        rhs.back_node = nullptr;
        rhs.node_count = 0;
    }

  public:
    struct list_node
    {
        E element;
        list_node *next;
        list_node(E element)
            : element(element){};
    };
    struct list_info_node
    {
        char data[sizeof(list_node) - sizeof(list_node *)];
        list_node *next;
        list_info_node(){};
    };
    static_assert(sizeof(list_node) == sizeof(list_info_node));

    struct iterator
    {
        friend class singly_linked_list;

      private:
        list_node *node;

        iterator(list_node *node)
            : node(node){};

      public:
        E &operator*() { return node->element; }
        E *operator&() { return &node->element; }
        E *operator->() { return &node->element; }

        iterator &operator++()
        {
            node = node->next;
            return *this;
        }
        iterator operator++(int)
        {
            auto old = *this;
            node = node->next;
            return old;
        }

        bool operator==(const iterator &it) { return it.node == node; }
        bool operator!=(const iterator &it) { return !operator==(it); }
    };
};
} // namespace util