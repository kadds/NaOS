#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/id_defined.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"

namespace syscall
{
file_desc open(const char *filename, u64 mode) { return 0; }

bool close(file_desc fd) { return 0; }

u64 write(file_desc fd, byte *buffer, u64 max_len) { return 0; }

u64 read(file_desc fd, byte *buffer, u64 max_len) { return 0; }

u64 lseek(file_desc fd, u64 offset, u64 type) { return 0; }

BEGIN_SYSCALL
SYSCALL(10, open)
SYSCALL(11, close)
SYSCALL(12, write)
SYSCALL(13, read)
SYSCALL(14, lseek)
END_SYSCALL

} // namespace syscall