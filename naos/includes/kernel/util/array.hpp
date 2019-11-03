#pragma once
#include "../mm/allocator.hpp"
#include "common.hpp"
#include "kernel/util/memory.hpp"

namespace util
{
template <typename E> class array
{
  private:
    E *buffer;
    memory::IAllocator *allocator;
    u64 count;
    u64 capacity;

  public:
    struct iterator
    {
        E *current;
        friend class array;

      private:
        explicit iterator(E *e)
            : current(e)
        {
        }
        /// 'explicit' keyword for avoid overload error
        explicit iterator(E &e)
            : current(&e)
        {
        }

      public:
        E operator*() { return *current; }
        E *operator&() { return current; }
        E *operator->() { return current; }

        iterator operator++(int) { return iterator(current + 1); }
        iterator &operator++()
        {
            current++;
            return *this;
        }
        iterator operator--(int) { return iterator(current - 1); }
        iterator &operator--()
        {
            current--;
            return *this;
        }
        bool operator==(const iterator &it) { return it.current == current; }

        bool operator!=(const iterator &it) { return !operator==(it); }
    };

    iterator push_back(const E &e)
    {
        count++;
        check_capacity();
        buffer[count - 1] = e;
        return iterator(buffer[count]);
    }

    iterator push_front(const E &e) { return insert(0, e); }

    E pop_back()
    {
        E e = buffer[--count];
        check_capacity();
        return e;
    }

    E pop_front()
    {
        E e = buffer[0];
        remove(0);
        check_capacity();
        return e;
    }

    // insert before
    void insert(u64 index, const E &e)
    {
        count++;
        check_capacity();
        for (u64 i = count; i > index; i--)
        {
            buffer[i] = buffer[i - 1];
        }
        buffer[0] = e;
        return begin();
    }
    /// insert before iter
    void insert(iterator iter, const E &e)
    {
        u64 index = iter.current - buffer;
        insert(index, e);
    }
    /// remove at index
    iterator remove(int index)
    {
        for (u64 i = index; i < count; i++)
        {
            buffer[i] = buffer[i + 1];
        }
        count--;
        check_capacity();
        return iterator(buffer[index]);
    }
    /// remove current iter
    iterator remove(iterator iter)
    {
        u64 index = iter.current - buffer;
        return remove(index);
    }

    bool empty() const { return count == 0; }

    u64 size() const { return count; }

    iterator begin() const { return iterator(buffer[0]); }

    iterator end() const { return iterator(buffer[count]); }

    E front() const { return *begin(); }
    E back() const { return buffer[count - 1]; }

    E at(u64 index) const { return buffer[index]; }

    E &at(u64 index) { return buffer[index]; }

    E &operator[](u64 index) { return at(index); }

    void fitcapcity() { recapcity(count); }

    void recapcity(u64 element_count)
    {
        E *new_buffer = (E *)allocator->allocate(element_count * sizeof(E), alignof(E));
        if (buffer != nullptr)
        {
            u64 c = element_count > count ? count : element_count;

            for (u64 i = 0; i < c; i++)
            {
                new_buffer[i] = buffer[i];
            }
            allocator->deallocate(buffer);
        }
        buffer = new_buffer;
        capacity = element_count;
    }

    void expand(u64 element_count)
    {
        if (unlikely(element_count <= count))
            return;
        recapcity(element_count);
    }

    void shrink(u64 element_count)
    {
        if (unlikely(buffer == nullptr))
            return;
        if (unlikely(element_count <= count))
            return;
        recapcity(element_count);
    }

    array(memory::IAllocator *allocator)
        : buffer(nullptr)
        , allocator(allocator)
        , count(0)
        , capacity(0)
    {
    }

  private:
    void check_capacity()
    {
        if (unlikely(capacity == 0))
        {
            expand(2);
            return;
        }
        if (unlikely(count >= capacity))
        {
            expand(capacity * 3 / 2);
        }
        else if (unlikely(count <= capacity * 2))
        {
            shrink(count);
        }
    }
};

} // namespace util
