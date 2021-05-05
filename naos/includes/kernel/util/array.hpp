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

    template <typename N> struct value_fn
    {
        N operator()(N val) { return val; }
    };
    template <typename N> struct prev_fn
    {
        N operator()(N val) { return val - 1; }
    };
    template <typename N> struct next_fn
    {
        N operator()(N val) { return val + 1; }
    };

    using CE = const E *;
    using NE = E *;

  public:
    using const_iterator = base_bidirectional_iterator<CE, value_fn<CE>, prev_fn<CE>, next_fn<CE>>;
    using iterator = base_bidirectional_iterator<NE, value_fn<NE>, prev_fn<NE>, next_fn<NE>>;

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
        {
            new (buffer + i) E(a);
            i++;
        }
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

    template <typename... Args> iterator push_back(Args &&...args)
    {
        check_capacity(count + 1);
        cassert(cap > count);
        new (buffer + count) E(std::forward<Args>(args)...);
        count++;
        return iterator(buffer + count - 1);
    }

    template <typename... Args> iterator push_front(Args &&...args)
    {
        return insert_at(0, std::forward<Args>(args)...);
    }

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
    template <typename... Args> iterator insert_at(u64 index, Args &&...args)
    {
        cassert(index <= count);
        check_capacity(count + 1);
        if (likely(count > 0))
        {
            new (buffer + count) E(std::move(buffer[count - 1]));
            for (u64 i = count - 1; i > index; i--)
                buffer[i] = std::move(buffer[i - 1]);
        }

        new (buffer + index) E(std::forward<Args>(args)...);
        count++;
        return iterator(buffer + index);
    }

    /// insert before iter
    template <typename... Args> iterator insert(iterator iter, Args &&...args)
    {
        u64 index = iter.get() - buffer;
        return insert_at(index, std::forward<Args>(args)...);
    }

    /// remove at index
    iterator remove_at(u64 index)
    {
        if (unlikely(index >= count))
            return end();

        for (u64 i = index; i + 1 < count; i++)
            buffer[i] = std::move(buffer[i + 1]);

        buffer[count - 1].~E();

        check_capacity(count - 1);
        count--;
        return iterator(&buffer[index]);
    }

    /// remove current iter
    iterator remove(iterator iter)
    {
        u64 index = iter.get() - buffer;
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

    template <typename... Args> void expand(u64 element_count, Args &&...args)
    {
        if (unlikely(element_count <= count))
            return;

        recapcity(element_count);
        for (u64 i = count; i < element_count; i++)
            new (&buffer[i]) E(std::forward<Args>(args)...);

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

    void remove_at(u64 index, u64 end_index)
    {
        cassert(index >= 0 && index <= end_index && end_index <= count);
        u64 rm_cnt = end_index - index;

        for (u64 i = index; i + rm_cnt < count; i++)
            buffer[i] = std::move(buffer[i + rm_cnt]);

        for (u64 i = 1; i <= rm_cnt; i++)
            buffer[count - i].~E();

        check_capacity(count - rm_cnt);
        count -= rm_cnt;
    }

    /// remove current iter
    iterator remove(iterator beg, iterator end)
    {
        u64 index = beg.get() - buffer;
        u64 end_index = end.get() - buffer;
        return remove_at(index, end_index);
    }

    void clear() { truncate(0); }

    E *data() { return buffer; }

    const E *data() const { return buffer; }

  private:
    void check_capacity(u64 new_count)
    {
        if (unlikely(cap == 0 && new_count > 0))
            recapcity(2);
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
            buffer[i] = rhs.buffer[i];
    }

    void recapcity(u64 element_count)
    {
        if (element_count == cap)
            return;

        E *new_buffer = reinterpret_cast<E *>(allocator->allocate(element_count * sizeof(E), alignof(E)));
        if (buffer != nullptr)
        {
            u64 c = element_count > count ? count : element_count;

            for (u64 i = 0; i < c; i++)
                new (new_buffer + i) E(std::move(buffer[i]));

            allocator->deallocate(buffer);
        }
        buffer = new_buffer;
        cap = element_count;
    }
};

} // namespace util
