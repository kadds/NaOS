#include "kernel/dev/tty/console_pseudo.hpp"

namespace dev::tty
{
i64 console_pseudo_t::write(const byte *data, u64 size, flag_t flags)
{
    (void)flags;
    if (data == nullptr)
        return EINVAL;
    term::write_to(
        freelibcxx::const_string_view(reinterpret_cast<const char *>(data), static_cast<u64>(size)),
        term_index_);
    return static_cast<i64>(size);
}

i64 console_pseudo_t::read(byte *, u64, flag_t)
{
    return 0;
}

void console_pseudo_t::close() {}
} // namespace dev::tty
