#include "kernel/dev/tty/tty.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

namespace dev::tty
{
using namespace fs;

bool tty_read_func(u64 data)
{
    auto *tty = (tty_pseudo_t *)data;
    auto buffer = &tty->buffer;
    return tty->eof_count > 0 || (tty->input_chars > 0 && !buffer->is_emtpy() && tty->enter > 0);
}

/// print to screen
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
        auto ch = (char)data[i];
        if (ch == '\n')
        {
            input_chars++;
            enter++;
        }

        if (buffer.is_full())
        {
            byte p = (byte)0;
            buffer.last(&p);
            if ((char)p == '\n')
                input_chars--;
        }
        if (ch == '\b')
        {
        }
        buffer.write(data[i]);
    }
    // trace::print_inner((const char *)data, size);
    if (enter > 0)
    {
        wait_queue.do_wake_up();
    }

    return size;
}

void tty_pseudo_t::send_EOF()
{
    eof_count++;
    wait_queue.do_wake_up();
}

i64 tty_pseudo_t::read(byte *data, u64 max_size, flag_t flags)
{
    if ((input_chars == 0 || enter == 0) && eof_count == 0)
    {
        if (flags & rw_flags::no_block)
        {
            return -1;
        }
        wait_queue.do_wait(tty_read_func, (u64)this);
    }
    for (u64 i = 0; i < max_size;)
    {
        if (buffer.is_emtpy())
        {
            if (i > 0)
                return i;
            if (flags & rw_flags::no_block)
                return i;
            if (eof_count > 0)
            {
                int old;
                bool ok = false;
                do
                {
                    old = eof_count;
                    if (old == 0)
                    {
                        ok = true;
                        break;
                    }
                } while (!eof_count.compare_exchange_strong(old, old - 1, std::memory_order_acquire));
                if (!ok)
                    return -1;
            }
            wait_queue.do_wait(tty_read_func, (u64)this);
        }
        char ch = '\0';
        buffer.read((byte *)&ch);
        if (ch == '\n') // keep \n
        {
            data[i++] = (byte)ch;
            enter--;
            input_chars--;
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

void tty_pseudo_t::close()
{
    wait_queue.remove(task::current_process());
    wait_queue.do_wake_up();
};

} // namespace dev::tty
