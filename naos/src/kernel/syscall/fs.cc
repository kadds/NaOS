#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/types.hpp"

namespace syscall
{
u64 access(const char *pathname) {}

u64 mkdir(const char *pathname, u64 attr)
{
    fs::vfs::mkdir(pathname, attr);
    return 0;
}

u64 rmdir(const char *pathname)
{
    fs::vfs::rmdir(pathname);
    return 0;
}

u64 create(const char *pathname, u64 mode) { return 0; }

u64 del(const char *pathname) { return 0; }

void chdir(const char *pathname) {}

void current_dir(const char *pathname, const u64 max_len) {}

void chroot(const char *pathname) {}

void set_attr(file_desc fd, const char *name, void *value, u64 size, u64 flags) {}

void get_attr(file_desc fd, const char *name, void *value, u64 max_size) {}

void link() {}
void unlink() {}

void mount() {}
void unmount() {}

BEGIN_SYSCALL
SYSCALL(25, mkdir);
SYSCALL(26, rmdir);
END_SYSCALL

} // namespace syscall