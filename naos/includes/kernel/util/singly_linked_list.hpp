#pragma once
#include "../mm/allocator.hpp"
#include "../mm/new.hpp"
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
        char data[sizeof(list_node) - sizeof(list_node *)];
        list_node *next;
        list_info_node(){};
    };
    using ElementType = E;

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
        iterator operator++(int) { return iterator(node->next); }

        bool operator==(const iterator &it) { return it.node == node; }
        bool operator!=(const iterator &it) { return !operator==(it); }
    };

  private:
    memory::IAllocator *allocator;

    list_info_node *head, *tail;
    list_node *back_node;
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

    iterator before_iterator(iterator start, iterator target)
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
        node->next = (list_node *)tail;
        back_node->next = node;
        node_count++;
        back_node = node;
        return iterator(node);
    }

    E pop_back()
    {
        if (unlikely(node_count <= 0))
            return E();
        auto iter = before_iterator(iterator((list_node *)head), iterator(back_node));
        iter->next = tail;
        E e = back_node->element;
        memory::Delete<list_node>(allocator, back_node);
        back_node = iter.node;
        node_count--;
        return e;
    }

    iterator push_front(const E &e)
    {
        list_node *node = memory::New<list_node>(allocator, e);
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

    iterator begin() const { return iterator(head->next); }

    iterator end() const { return iterator((list_node *)tail); }

    bool empty() const { return head->next == (list_node *)tail; }

    E back() const { return back_node->element; }

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

        node->next = prev_node->next;
        prev_node->next = node;

        if (iter->next->next == (list_node *)tail)
        {
            back_node = iter->next;
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
        , back_node(nullptr)
        , node_count(0)
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

    singly_linked_list(const singly_linked_list &l)
        : allocator(l.allocator)
        , head(memory::New<list_info_node>(allocator))
        , tail(memory::New<list_info_node>(allocator))
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

    singly_linked_list &operator=(const singly_linked_list &l)
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
        node_count = 0;

        auto iter = l.begin();
        while (iter != l.end())
        {
            push_back(*iter);
            iter++;
        }
        return *this;
    }
};
} // namespace util