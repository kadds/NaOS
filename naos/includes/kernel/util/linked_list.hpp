#pragma once
#include "../mm/new.hpp"
#include "common.hpp"

namespace util
{
template <typename E> class linked_list
{
  public:
    struct list_node
    {
        E element;
        list_node *prev, *next;
        explicit list_node(E element)
            : element(element){};

        template <typename... Args>
        list_node(Args &&... args)
            : element(std::forward<Args>(args)...){};
    };
    struct list_info_node
    {
        char data[sizeof(list_node) - sizeof(list_node *) * 2];
        list_node *prev, *next;
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

        iterator &operator--()
        {
            node = node->prev;
            return *this;
        }

        iterator operator--(int) { return iterator(node->prev); }

        bool operator==(const iterator &it) { return it.node == node; }
        bool operator!=(const iterator &it) { return !operator==(it); }
    };

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
    iterator push_back(const E &e)
    {
        list_node *node = memory::New<list_node>(allocator, e);
        node->next = (list_node *)tail;
        tail->prev->next = node;
        node->prev = tail->prev;
        tail->prev = node;
        node_count++;
        return iterator(node);
    }

    template <typename... Args> iterator emplace_back(Args &&... arg)
    {
        list_node *node = memory::New<list_node>(allocator, std::forward<Args>(arg)...);
        node->next = (list_node *)tail;
        tail->prev->next = node;
        node->prev = tail->prev;
        tail->prev = node;
        node_count++;
        return iterator(node);
    }

    iterator push_front(const E &e)
    {
        list_node *node = memory::New<list_node>(allocator, e);
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

    iterator begin() const { return iterator(head->next); }

    iterator end() const { return iterator((list_node *)tail); }

    iterator rbegin() const { return iterator(((list_node *)tail)->prev); }

    iterator rend() const { return iterator((list_node *)head); }

    bool empty() const { return head->next == (list_node *)tail; }

    const E &back() const { return tail->prev->element; }

    const E &front() const { return head->next->element; }

    E &back() { return tail->prev->element; }

    E &front() { return head->next->element; }

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
    iterator insert(iterator iter, const E &e)
    {
        list_node *after_node = iter.node;
        list_node *node = memory::New<list_node>(allocator, e);
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
        list_node *node = iter.node;
        list_node *next = node->next;
        node->prev->next = node->next;
        node->next->prev = node->prev;
        node_count--;
        memory::Delete<>(allocator, node);
        return iterator(next);
    }

    void clean()
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

    ~linked_list()
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
    {
        head->next = (list_node *)tail;
        head->prev = nullptr;
        tail->prev = (list_node *)head;
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
        // clean previous data
        list_node *node = (list_node *)head;
        while (node != nullptr)
        {
            list_node *node2 = node->next;
            memory::Delete<>(allocator, node);
            node = node2;
        }

        // gen new data
        allocator = l.allocator;
        head = memory::New<list_info_node>(allocator);
        tail = memory::New<list_info_node>(allocator);

        head->next = (list_node *)tail;
        head->prev = nullptr;
        tail->prev = (list_node *)head;
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