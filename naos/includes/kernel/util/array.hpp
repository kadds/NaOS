#pragma once
#include "../mm/new.hpp"
#include "assert.hpp"
#include "common.hpp"
#include "iterator.hpp"
#include <utility>

namespace util
{
/// A container like std::vector
template <typename E> class array
{
  private:
    E *buffer;
    u64 count;
    u64 cap;
    memory::IAllocator *allocator;

  public:
    using const_iterator = base_bidirectional_iterator<const E *>;
    using iterator = base_bidirectional_iterator<E *>;

    array(memory::IAllocator *allocator)
        : buffer(nullptr)
        , count(0)
        , cap(0)
        , allocator(allocator)
    {
    }

    array(memory::IAllocator *allocator, std::initializer_list<E> il)
        : array(allocator)
    {
        recapcity(il.size());
        count = il.size();
        u64 i = 0;
        for (E a : il)
            buffer[i++] = a;
    }

    array(const array &rhs) { copy(rhs); }

    array(array &&rhs) { move(std::move(rhs)); }

    ~array() { free(); }

    array &operator=(const array &rhs)
    {
        if (&rhs == this)
            return *this;
        free();
        copy(rhs);
        return *this;
    }

    array &operator=(array &&rhs)
    {
        if (this == &rhs)
            return *this;
        free();
        move(std::move(rhs));
        return *this;
    }

    iterator push_back(E &&e)
    {
        check_capacity(count + 1);
        cassert(cap > count);
        buffer[count] = std::move(e);
        return iterator(&buffer[++count]);
    }

    iterator push_front(E &&e) { return insert_at(0, std::move(e)); }

    E pop_back()
    {
        cassert(count > 0);
        E e = std::move(buffer[count - 1]);
        remove_at(count - 1);
        return e;
    }

    E pop_front()
    {
        cassert(count > 0);
        E e = std::move(buffer[0]);
        remove_at(0);
        return e;
    }

    E &back() {
        cassert(count > 0);
        return buffer[count - 1];
    }
    E &front() {
        cassert(count > 0);
        return buffer[0];
    }

    const E &back() const {
        cassert(count > 0);
        return buffer[count - 1];
    }

    const E &front() const {
        cassert(count > 0);
        return buffer[0];
    }

    // insert before
    iterator insert_at(u64 index, E &&e)
    {
        cassert(index <= count);
        check_capacity(count + 1);
        for (u64 i = count; i > index; i--)
        {
            buffer[i] = std::move(buffer[i - 1]);
        }
        buffer[index] = std::move(e);
        count++;
        return iterator(&buffer[index]);
    }

    /// insert before iter
    iterator insert(iterator iter, E &&e)
    {
        u64 index = &iter - buffer;
        return insert_at(index, std::move(e));
    }

    /// remove at index
    iterator remove_at(u64 index)
    {
        if (unlikely(index >= count))
            return end();

        buffer[index].~E();
        for (u64 i = index; i + 1 < count; i++)
        {
            buffer[i] = std::move(buffer[i + 1]);
        }
        check_capacity(count - 1);
        count--;
        return iterator(&buffer[index]);
    }

    /// remove current iter
    iterator remove(iterator iter)
    {
        u64 index = &iter - buffer;
        return remove_at(index);
    }

    bool empty() const { return count == 0; }

    u64 size() const { return count; }

    u64 capacity() const { return cap; }

    const_iterator begin() const { return const_iterator(buffer); }

    const_iterator end() const { return const_iterator(buffer + count); }

    iterator begin() { return iterator(buffer); }

    iterator end() { return iterator(buffer + count); }

    E &at(u64 index) {
        cassert(index < count);
        return buffer[index]; 
    }

    const E & at(u64 index) const { 
        cassert(index < count);
        return buffer[index]; 
    }

    E &operator[](u64 index) { return at(index); }

    void fitcapcity() { recapcity(count); }

    void expand(u64 element_count, E &&val)
    {
        if (unlikely(element_count <= count))
            return;
        recapcity(element_count);
        for (u64 i = count; i < element_count; i++)
        {
            new (&buffer[i]) E(std::move(val));
        }
        count = element_count;
    }

    void ensure(u64 size) {
        if (size > cap)
        {
            recapcity(size);
        }
    }

    void shrink(u64 element_count)
    {
        if (unlikely(element_count >= count))
            return;
        recapcity(element_count);
    }

    void resize(u64 size, E &&val)
    {
        if (unlikely(size < count))
            truncate(size);
        else if (unlikely(size == count))
            return;
        else
            expand(size, std::move(val));
    }

    void truncate(u64 size)
    {
        for (u64 i = size; i < count; i++)
            buffer[i].~E();
        count = size;
    }

    void clear() { truncate(0); }

    E *data() { return buffer; }

    const E *data() const { return buffer; }

  private:
    void check_capacity(u64 new_count)
    {
        if (unlikely(cap == 0 && new_count > 0))
        {
            recapcity(2);
        }
        else if (unlikely(new_count > cap))
        {
            u64 x = cap * 3 / 2;
            if (unlikely(x < new_count))
                x = new_count;

            recapcity(x);
        }
        // else if (unlikely(new_count + 2 <= cap * 2))
        // {
        //     recapcity(new_count);
        // }
    }

    void free()
    {
        if (buffer != nullptr)
        {
            truncate(0);
            allocator->deallocate(buffer);
            buffer = nullptr;
            cap = 0;
            // size = 0;
        }
    }

    void move(array &&rhs)
    {
        buffer = rhs.buffer;
        count = rhs.count;
        cap = rhs.cap;
        allocator = rhs.allocator;
        rhs.buffer = nullptr;
        rhs.count = 0;
        rhs.cap = 0;
    }

    void copy(const array &rhs)
    {
        count = rhs.count;
        cap = rhs.count;
        allocator = rhs.allocator;
        buffer = reinterpret_cast<E *>(allocator->allocate(count * sizeof(E), alignof(E)));
        for (u64 i = 0; i < count; i++)
        {
            buffer[i] = rhs.buffer[i];
        }
    }

    void recapcity(u64 element_count)
    {
        if (element_count == cap)
        {
            return;
        }
        E *new_buffer = reinterpret_cast<E *>(allocator->allocate(element_count * sizeof(E), alignof(E)));
        if (buffer != nullptr)
        {
            u64 c = element_count > count ? count : element_count;

            for (u64 i = 0; i < c; i++)
            {
                new_buffer[i] = std::move(buffer[i]);
            }
            allocator->deallocate(buffer);
        }
        buffer = new_buffer;
        cap = element_count;
    }
};

} // namespace util
