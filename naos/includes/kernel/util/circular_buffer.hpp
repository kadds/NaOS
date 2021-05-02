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
            /// XXX: no thread safety, may loss data
            if (cb->is_full())
            {
                cb->read_off++;
            }

            if (unlikely(cb->write_off + 1 >= cb->length))
            {
                cb->write_off = 0;
            }
            else
            {
                cb->write_off++;
            }
        }
    };

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

    write_helper write_with() { return write_helper(this); }

    u64 get_buffer_size() const { return length; }

    bool is_full() const { return (write_off + 1) % length == read_off; }

    bool is_emtpy() const { return read_off == write_off; }

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
