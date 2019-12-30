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
    struct write_helper
    {
        circular_buffer *cb;

        T &operator*() const { return cb->buffer[cb->write_off]; }

        write_helper &operator=(const T &t)
        {
            cb->buffer[cb->write_off] = t;
            return *this;
        }

        write_helper(circular_buffer *cb)
            : cb(cb)
        {
        }

        ~write_helper()
        {
            if (unlikely((cb->write_off + 1) == cb->read_off || cb->write_off == cb->read_off - 1))
            {
                cb->read_off++;
            }
            cb->write_off++;
            if (unlikely(cb->write_off >= cb->length))
            {
                cb->write_off = 0;
            }
        }
    };

    circular_buffer(memory::IAllocator *allocator, u64 count)
        : length(count)
        , read_off(0)
        , write_off(0)
        , allocator(allocator)
    {
        if (count > 0)
            buffer = (T *)allocator->allocate(count * sizeof(T), alignof(T));
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

    write_helper write_with() { return write_helper(this); }

    u64 data_size()
    {
        if (write_off >= read_off)
            return write_off - read_off;
        else
            return length - read_off + write_off;
    }

    u64 size() { return length; }

    bool is_full() { return (write_off + 1) % length == read_off; }

    void resize(u64 size)
    {
        /// TODO: when size < length
        if (size < length)
            return;
        T *new_buf;
        if (size > 0)
            new_buf = (T *)allocator->allocate(size * sizeof(T), alignof(T));
        else
            new_buf = nullptr;

        if (buffer != nullptr)
        {
            if (new_buf != nullptr)
            {
                memcopy(new_buf, buffer, length * sizeof(T));
            }
            allocator->deallocate(buffer);
        }
        buffer = new_buf;
        length = size;
    }

    bool read(T *t)
    {
        if (unlikely(read_off == write_off))
        {
            return false;
        }
        *t = buffer[read_off];
        read_off++;
        if (unlikely(read_off >= length))
        {
            read_off = 0;
        }
        return true;
    }
};

} // namespace util
