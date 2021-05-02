#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
#include "memory.hpp"

namespace util
{

template <typename T> class circular_buffer
{
  private:
    T *buffer;
    u64 length;
    u64 read_off;
    u64 write_off;
    memory::IAllocator *allocator;

  public:
    circular_buffer(memory::IAllocator *allocator, u64 size)
        : buffer(nullptr)
        , length(size)
        , read_off(0)
        , write_off(0)
        , allocator(allocator)
    {
        if (size > 0)
            buffer = reinterpret_cast<T *>(allocator->allocate(size * sizeof(T), alignof(T)));
        else
            buffer = nullptr;
    }

    ~circular_buffer()
    {
        if (buffer != nullptr)
            allocator->deallocate(buffer);
    }

    circular_buffer &operator=(const circular_buffer &cb) = delete;
    circular_buffer(const circular_buffer &cb) = delete;

    void write(const T &t) { write(&t, 1); }

    void write(const T *t, u64 len)
    {
        u64 max_off = write_off + len;
        u64 rest_off = 0;
        u64 new_write_off = max_off;
        if (max_off > length)
        {
            max_off = length;
            rest_off = write_off + len - length;
            new_write_off = rest_off;
        }

        for (u64 off = write_off; off < max_off; off++, t++)
        {
            buffer[off] = *t;
        }

        for (u64 off = 0; off < rest_off; off++, t++)
        {
            buffer[off] = *t;
        }

        write_off = new_write_off;
    }

    u64 get_buffer_size() const { return length; }

    bool is_full() const { return (write_off + 1) % length == read_off; }

    bool is_emtpy() const { return read_off == write_off; }

    bool last(T *t)
    {
        if (unlikely(is_emtpy()))
        {
            return false;
        }
        u64 off = write_off;
        if (off == 0)
        {
            off = length;
        }
        *t = buffer[off - 1];
        return true;
    }

    bool read(T *t)
    {
        if (unlikely(is_emtpy()))
        {
            return false;
        }
        *t = buffer[read_off];
        if (read_off + 1 >= length)
        {
            read_off = 0;
        }
        else
        {
            read_off++;
        }
        return true;
    }
};

} // namespace util
