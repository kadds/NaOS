#pragma once

#include "kernel/dev/tty/pty.hpp"
#include "kernel/fs/vfs/vfs.hpp"

namespace dev::pty
{
void init();

handle_t<fs::vfs::file> open_master(flag_t mode);
handle_t<fs::vfs::file> open_slave(u32 index, flag_t mode);
} // namespace dev::pty
