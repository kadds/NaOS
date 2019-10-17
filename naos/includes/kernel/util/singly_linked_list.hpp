#pragma once
#include "../mm/allocator.hpp"
#include "common.hpp"

namespace util
{
template <typename E> class singly_linked_list
{
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
        char data[sizeof(E)];
        list_node *next;
        list_info_node(){};
    };
    using ElementType = E;

    static_assert(sizeof(list_node) == sizeof(list_info_node));

    struct iterator
    {
        friend class linked_list;

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
        iterator operator++(int) { return iterator(node->next); }

        bool operator==(const iterator &it) { return it.node == node; }
        bool operator!=(const iterator &it) { return !operator==(it); }
    };

  private:
    memory::IAllocator *allocator;

    list_info_node *head, *tail;
    list_info_node *back_node;
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

    iterator before_iterator(iterator start = iterator((list_node *)head), iterator target = end())
    {
        auto iter = start;
        do
        {
            auto next = iter;
            if (++next == target)
                break;
        } while (true);
        return iter;
    }

  public:
    iterator push_back(const E &e)
    {
        list_node *node = memory::New<list_node>(allocator, e);
        list_node *prev;

        if (unlikely(back_node == nullptr))
            prev = head;
        else
            prev = back_node;

        node->next = (list_node *)tail;
        prev->next = node;
        back_node = node;
        node_count++;
        return iterator(node);
    }

    iterator push_front(const E &e)
    {
        list_node *node = memory::New<list_node>(allocator, e);
        node->next = head->next;
        head->next = node;
        node_count++;
        return iterator(node);
    }

    E pop_back()
    {
        E e = tail->prev->element;
        iterator iter = before_iterator(iterator((list_node *)head), back_node);
        list_node *new_back_node = *iter;
        new_back_node->next = back_node->next;
        memory::Delete<>(allocator, back_node);
        back_node = new_back_node;
        node_count--;
        return e;
    };

    E pop_front()
    {
        E e = head->next->element;
        list_node *node = head->next;
        head->next = head->next->next;
        memory::Delete<>(allocator, node);
        node_count--;
        return e;
    };

    u64 size() const
    {
        kassert(node_count == calc_size(), "node count != ", node_count);
        return node_count;
    }

    iterator begin() const { return iterator(head->next); }

    iterator end() const { return iterator((list_node *)tail); }

    bool empty() const { return head->next == (list_node *)tail; }

    E back() const { return tail->prev->element; }

    E front() const { return head->next->element; }

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
    iterator insert(iterator iter, const E &e) { insert_after(before_iterator((list_node *)head, iter), e); }

    iterator insert_after(iterator iter, const E &e)
    {
        list_node *prev_node = *iter;
        list_node *node = memory::New<list_node>(allocator, e);
        auto last = after_node->prev;

        node->next = prev_node->next;
        prev_node->next = node;

        node_count++;
        return iterator(node);
    }

    iterator remove(iterator iter)
    {
        if (unlikely(iter == end() || iter == iterator((list_node *)head)))
            return end();
        list_node *prev = before_iterator((list_node *)head, iter).node;
        prev->next = iter.node->next;
        node_count--;
        memory::Delete<>(allocator, iter.node);
        return iterator(next);
    }

    iterator remove(iterator prev, iterator iter)
    {
        if (unlikely(iter == end() || iter == iterator((list_node *)head)))
            return end();
        list_node *prev_node = prev.node;
        if (prev_node->next != iter.node)
            return end();

        prev_node->next = iter.node->next;
        node_count--;
        memory::Delete<>(allocator, iter.node);
        return iterator(next);
    }

    void clean()
    {
        auto it = begin();
        while (it != end())
        {
            auto cur = it++;
            memory::Delete<>(allocator, *cur);
        }
    }

    singly_linked_list(memory::IAllocator *allocator)
        : allocator(allocator)
        , head(memory::New<list_info_node>(allocator))
        , tail(memory::New<list_info_node>(allocator))
        , node_count(0)
        , back_node(nullptr)
    {
        head->next = (list_node *)tail;
        tail->next = nullptr;
    };

    ~singly_linked_list()
    {
        list_node *node = (list_node *)head;
        while (node != nullptr)
        {
            list_node *node2 = node->next;
            memory::Delete<>(allocator, node);
            node = node2;
        }
    }

    linked_list(const linked_list &l)
        : allocator(l.allocator)
        , head(memory::New<list_info_node>(allocator))
        , tail(memory::New<list_info_node>(allocator))
        , back_node(nullptr)
    {
        head->next = (list_node *)tail;
        tail->next = nullptr;

        auto iter = l.begin();
        while (iter != l.end())
        {
            push_back(*iter);
            iter++;
        }
    }

    linked_list &operator=(const linked_list &l)
    {
        if (&l == this)
            return *this;

        list_node *node = (list_node *)head;
        while (node != nullptr)
        {
            list_node *node2 = node->next;
            memory::Delete<>(allocator, node);
            node = node2;
        }

        allocator = l.allocator;
        head = memory::New<list_info_node>(allocator);
        tail = memory::New<list_info_node>(allocator);
        head->next = (list_node *)tail;
        tail->next = nullptr;

        auto iter = l.begin();
        while (iter != l.end())
        {
            push_back(*iter);
            iter++;
        }
        return *this;
    }
};
}