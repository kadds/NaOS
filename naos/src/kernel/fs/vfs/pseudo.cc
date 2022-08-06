#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/defines.hpp"
namespace fs::vfs
{

bool pipe_write_func(u64 data)
{
    auto *pipe = (pseudo_pipe_t *)data;
    return pipe->is_close || !pipe->buffer.full();
}

bool pipe_read_func(u64 data)
{
    auto *pipe = (pseudo_pipe_t *)data;
    return pipe->is_close || !pipe->buffer.empty();
}

i64 pseudo_pipe_t::write(const byte *data, u64 size, flag_t flags)
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
            wait_queue.do_wake_up();
            wait_queue.do_wait(pipe_write_func, (u64)this);
        }
        buffer.write(data[i]);
    }
    wait_queue.do_wake_up();
    return size;
}

i64 pseudo_pipe_t::read(byte *data, u64 max_size, flag_t flags)
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
            wait_queue.do_wake_up();
            wait_queue.do_wait(pipe_read_func, (u64)this);
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