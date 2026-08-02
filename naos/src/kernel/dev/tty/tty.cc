#include "kernel/dev/tty/tty.hpp"

#include "freelibcxx/string.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/terminal.hpp"

namespace dev::tty
{
using namespace fs;

bool tty_read_func(u64 data)
{
    auto *tty = reinterpret_cast<tty_pseudo_t *>(data);
    return tty != nullptr && tty->readable();
}

i64 tty_pseudo_t::write(const byte *data, u64 size, flag_t flags)
{
    const auto result = core_.write_output(data, size, flags);
    render_master_output();
    return result;
}

i64 tty_pseudo_t::read(byte *data, u64 max_size, flag_t flags) { return core_.read_input(data, max_size, flags); }

u64 tty_pseudo_t::write_to_buffer(const byte *data, u64 size, flag_t flags)
{
    const auto result = master_.write(data, size, flags);
    render_master_output();
    return result < 0 ? 0 : static_cast<u64>(result);
}

void tty_pseudo_t::send_EOF()
{
    core_.send_eof();
    render_master_output();
}

i64 tty_pseudo_t::ioctl(fs::vfs::ioctl_context &context) { return core_.ioctl(context); }

void tty_pseudo_t::render_master_output()
{
    byte buffer[128];
    tty_output_source sources[128];
    while (true)
    {
        const auto count = master_.read(buffer, sizeof(buffer), fs::rw_flags::no_block, sources);
        if (count <= 0)
            break;

        u64 offset = 0;
        while (offset < static_cast<u64>(count))
        {
            const auto source = sources[offset];
            u64 end = offset + 1;
            while (end < static_cast<u64>(count) && sources[end] == source)
                end++;

            term::write_to(freelibcxx::const_string_view(reinterpret_cast<const char *>(buffer + offset), end - offset),
                           term_index_);
            bool ends_line = false;
            for (u64 index = offset; index < end; index++)
            {
                if (static_cast<u8>(buffer[index]) == '\n')
                {
                    ends_line = true;
                    break;
                }
            }
            if (source == tty_output_source::normal || ends_line)
                term::commit_changes(term_index_);
            offset = end;
        }
    }
}

// The framebuffer console is a persistent device node. Closing the temporary
// setup handle (or the last inherited fd) must not permanently hang up the
// console core; PTY endpoints own the actual hangup transition.
void tty_pseudo_t::close() {}

} // namespace dev::tty
