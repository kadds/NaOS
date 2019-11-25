#pragma once
#include "common.hpp"
#include "memory.hpp"
namespace util
{
class ring_buffer
{
  private:
    byte *buffer;
    u64 capacity;
    u64 position;

  public:
    ring_buffer(void *buffer, u64 capacity)
        : buffer((byte *)buffer)
        , capacity(capacity)
        , position(0)
    {
    }

    u64 write(const void *data, u64 size)
    {
        u64 s = size;
        u64 rest = capacity - position;
        byte *d = (byte *)data;
        if (size < rest)
        {
            memcopy(buffer + position, d, size);
            position += size;
            return size;
        }
        else
        {
            memcopy(buffer + position, d, rest);
            position = 0;
            d += size;
            size -= rest;
        }

        u64 pice = size / capacity;
        for (u64 i = 0; i < pice; i++, d += capacity, size -= capacity)
            memcopy(buffer, d, capacity);

        memcopy(buffer, d, size);
        position += size;
        return s;
    }

    u64 read(void *dst, u64 offset, u64 len) const
    {
        if (offset >= capacity)
            return 0;
        if (offset <= capacity - position)
        {
            u64 slen = capacity - offset - position;
            if (slen > len)
                slen = len;
            memcopy(dst, buffer + position + offset, slen);
            len -= slen;
            dst = ((byte *)dst) + slen;
        }
        if (len > position)
            len = position;

        memcopy(dst, buffer, len);
        return len;
    }
    u64 get_capacity() const { return capacity; }
    const byte *get_buffer() const { return buffer; }
};
} // namespace util
