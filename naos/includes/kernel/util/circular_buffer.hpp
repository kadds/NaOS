#include "../mm/new.hpp"
#include "common.hpp"
namespace util
{

template <typename T> class circular_buffer
{
  private:
    T *buffer;
    u64 length;
    u64 read_off;
    u64 data_valied;
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
            cb->write_off++;
            if (unlikely(cb->write_off >= cb->length))
            {
                cb->write_off = 0;
            }
            if (unlikely(cb->data_valied == cb->read_off))
            {
                cb->data_valied++;
                if (unlikely(cb->data_valied >= cb->length))
                {
                    cb->data_valied = 0;
                }
            }
        }
    };

    circular_buffer(memory::IAllocator *allocator, u64 count)
        : length(count)
        , read_off(0)
        , data_valied(0)
        , write_off(0)
    {
        buffer = (T *)allocator->allocate(count * sizeof(T), alignof(T));
    }

    ~circular_buffer()
    {
        if (buffer != nullptr)
            allocator->deallocate(buffer);
    }

    circular_buffer &operator=(const circular_buffer &cb) = delete;
    circular_buffer(const circular_buffer &cb) = delete;

    write_helper write_with() { return write_helper(this); }

    bool read(T *t)
    {
        if (unlikely(read_off == data_valied))
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
