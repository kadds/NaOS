#include "kernel/dev/tty/tty.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/trace.hpp"

namespace dev::tty
{
using namespace fs;

bool tty_read_func(u64 data)
{
    auto *tty = (tty_pseudo_t *)data;
    auto buffer = &tty->buffer;
    return tty->line_count > 0 && !buffer->is_emtpy();
}

i64 tty_pseudo_t::write(const byte *data, u64 size, flag_t flags)
{
    trace::print_inner((const char *)data, size);
    return size;
}

u64 tty_pseudo_t::write_to_buffer(const byte *data, u64 size, flag_t flags)
{
    trace::print_inner((const char *)data, size);
    for (u64 i = 0; i < size; i++)
    {
        if ((char)data[i] == '\n')
        {
            line_count++;
        }
        auto wh = buffer.write_with();

        if (buffer.is_full())
        {
            if ((char)*wh == '\n')
                line_count--;
        }
        wh = data[i];
    }
    task::do_wake_up(&wait_queue);

    return size;
}

i64 tty_pseudo_t::read(byte *data, u64 max_size, flag_t flags)
{
    if (line_count <= 0)
    {
        if (flags & rw_flags::no_block)
        {
            return -1;
        }
        task::do_wait(&wait_queue, tty_read_func, (u64)this, task::wait_context_type::interruptable);
    }
    for (u64 i = 0; i < max_size;)
    {
        if (buffer.is_emtpy())
        {
            if (i > 0)
                return i;
            if (flags & rw_flags::no_block)
                return i;
            task::do_wait(&wait_queue, tty_read_func, (u64)this, task::wait_context_type::interruptable);
        }
        char ch;
        buffer.read((byte *)&ch);
        if (ch == '\n')
        {
            line_count--;
            return i;
        }
        else if (ch == '\b')
        {
            if (i > 0)
                i--;
            continue;
        }
        data[i++] = (byte)ch;
    }
    return max_size;
}

void tty_pseudo_t::close() { task::do_wake_up(&wait_queue); };

} // namespace dev::tty
