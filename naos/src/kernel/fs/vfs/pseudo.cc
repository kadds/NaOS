#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/defines.hpp"
namespace fs::vfs
{

i64 pseudo_pipe_t::write(const byte *data, u64 size, flag_t flags)
{
    i64 offset = 0;
    return write_at_interruptible(offset, data, size, flags, nullptr, nullptr);
}

i64 pseudo_pipe_t::write_at_interruptible(i64 &, const byte *data, u64 size, flag_t flags,
                                           interruption_check interrupted, wait_queue_registration register_wait_queue)
{
    for (u64 i = 0; i < size; i++)
    {
        while ((buffer.full() && !(flags & rw_flags::override)) || is_close)
        {
            if (i > 0)
                return i;
            if (is_close)
                return -1;
            if (flags & rw_flags::no_block)
                return -1;
            if (interrupted != nullptr && interrupted())
                return EINTR;
            wait_queue.do_wake_up();
            if (register_wait_queue != nullptr)
                register_wait_queue(&wait_queue);
            wait_queue.do_wait([this, interrupted] {
                return is_close || !buffer.full() || (interrupted != nullptr && interrupted());
            });
            if (interrupted != nullptr && interrupted())
                return EINTR;
        }
        buffer.write(data[i]);
    }
    wait_queue.do_wake_up();
    return size;
}

i64 pseudo_pipe_t::read(byte *data, u64 max_size, flag_t flags)
{
    i64 offset = 0;
    return read_at_interruptible(offset, data, max_size, flags, nullptr, nullptr);
}

i64 pseudo_pipe_t::read_at_interruptible(i64 &, byte *data, u64 max_size, flag_t flags,
                                          interruption_check interrupted, wait_queue_registration register_wait_queue)
{
    for (u64 i = 0; i < max_size; i++)
    {
        while (buffer.empty())
        {
            if (i > 0)
                return i;
            if (is_close)
                return -1;
            if (flags & rw_flags::no_block)
                return -1;
            if (interrupted != nullptr && interrupted())
                return EINTR;
            wait_queue.do_wake_up();
            if (register_wait_queue != nullptr)
                register_wait_queue(&wait_queue);
            wait_queue.do_wait([this, interrupted] {
                return is_close || !buffer.empty() || (interrupted != nullptr && interrupted());
            });
            if (interrupted != nullptr && interrupted())
                return EINTR;
        }
        buffer.read(&data[i]);
    }
    wait_queue.do_wake_up();
    return max_size;
}

void pseudo_pipe_t::close()
{
    is_close = true;
    wait_queue.do_wake_up();
}

} // namespace fs::vfs
