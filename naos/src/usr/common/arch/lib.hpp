#pragma once
#include <cstddef>
#include <cstdint>

extern "C" char _lib_sys_call;

typedef int fd_t;

#define SYS_CALL(index, ret, name, ...)                                                                                \
    extern "C" ret name(__VA_ARGS__);                                                                                  \
    __asm__(".globl " #name " \n\t "                                                                                   \
            ".type	" #name ",	@function \n\t" #name ": \n\t"                                                         \
            "movq $" #index ", %rax \n\t"                                                                              \
            "jmp _lib_sys_call \n\t ");

SYS_CALL(0, void, sys_none, void)

#define OPEN_MODE_READ 1
#define OPEN_MODE_WRITE 2
#define OPEN_MODE_BIN 4
#define OPEN_MODE_APPEND 8

#define STDIN 0
#define STDOUT 1
#define STDERR 2

#define OPEN_ATTR_AUTO_CREATE_FILE 1

SYS_CALL(2, fd_t, open, const char *path, uint64_t mode, uint64_t attr)
SYS_CALL(3, void, close, fd_t fd);

#define RWFLAGS_NO_BLOCK 1
#define RWFLAGS_OVERRIDE 2

SYS_CALL(4, int64_t, write, fd_t fd, const char *buffer, uint64_t len, uint64_t flags)
SYS_CALL(5, int64_t, read, fd_t fd, char *buffer, uint64_t max_len, uint64_t flags)

#define LSEEK_MODE_CURRENT 0
#define LSEEK_MODE_BEGIN 1
#define LSEEK_MODE_END 2

SYS_CALL(6, int64_t, pwrite, fd_t fd, uint64_t offset, const char *buffer, uint64_t len, uint64_t flags)
SYS_CALL(7, int64_t, pread, fd_t fd, uint64_t offset, char *buffer, uint64_t max_len, uint64_t flags)

SYS_CALL(8, int64_t, lseek, fd_t fd, int64_t offset, int mode)

SYS_CALL(9, int64_t, select, uint64_t size, fd_t *rfd, fd_t *wfd, fd_t *errfd, uint64_t flags)

SYS_CALL(10, int64_t, get_pipe, fd_t *fd1, fd_t *fd2)
SYS_CALL(11, int, create_fifo, const char *path, uint64_t mode)

SYS_CALL(12, int64_t, fcntl, fd_t fd, unsigned int operator_type, unsigned int target, unsigned int attr, void *value,
         uint64_t size)

struct list_directory_cursor
{
    uint32_t size;
    int64_t cursor;
};

struct list_directory_result
{
    list_directory_cursor cursor;
    uint32_t num_files;
    char *files_buffer;
    uint32_t bytes;
    char *buffer;
};

SYS_CALL(16, int, list_directory, fd_t fd, list_directory_result *result);
SYS_CALL(17, int, rename, const char *src, const char *target);
SYS_CALL(18, int, symbolink, const char *src, const char *target, uint64_t flags);

SYS_CALL(19, int, create, const char *filepath)

#define ACCESS_MODE_READ 1
#define ACCESS_MODE_WRITE 2
#define ACCESS_MODE_EXEC 4
#define ACCESS_MODE_EXIST 8

SYS_CALL(20, int, access, int64_t mode)

SYS_CALL(21, int, mkdir, const char *path)
SYS_CALL(22, int, rmdir, const char *path)
SYS_CALL(23, int, chdir, const char *new_workpath)
SYS_CALL(24, int64_t, current_dir, char *path, uint64_t max_len)
SYS_CALL(25, int, chroot, const char *path)
SYS_CALL(26, int, link, const char *src, const char *target)
SYS_CALL(27, int, unlink, const char *target)

SYS_CALL(28, int, mount, const char *dev, const char *mount_point, const char *fs_type, uint64_t flags,
         const char *data, uint64_t size)
SYS_CALL(29, int, umount, const char *mount_point)

SYS_CALL(30, void, exit, int64_t ret)
SYS_CALL(31, void, sleep, uint64_t ms)
SYS_CALL(32, int64_t, current_pid)
SYS_CALL(33, int64_t, current_tid)

#define CP_FLAG_NORETURN 1
#define CP_FLAG_BINARY 2
#define CP_FLAG_SHARED_FILE 8
#define CP_FLAG_SHARED_NOROOT 16
#define CP_FLAG_SHARED_WORK_DIR 32

SYS_CALL(34, int64_t, create_process, const char *filename, char *const args[], char *const env[], uint64_t flags)

#define CT_FLAG_IMMEDIATELY 1
#define CT_FLAG_NORETURN 4

SYS_CALL(35, int64_t, create_thread, void *entry, uint64_t arg, uint64_t flags)
SYS_CALL(36, int, detach, int64_t tid)
SYS_CALL(37, int, join, int64_t tid, int64_t *ret)
SYS_CALL(38, int64_t, wait_process, int64_t pid, int64_t *ret)
SYS_CALL(39, [[noreturn]] void, exit_thread, int64_t ret)

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHILD 17
#define SIGCOUT 18
#define SIGSTOP 19
#define SIGPWR 30
#define SIGSYS 31
// more ...

#define SIGOPT_GET 1
#define SIGOPT_SET 2
#define SIGOPT_OR 3
#define SIGOPT_AND 4
#define SIGOPT_XOR 5
#define SIGOPT_INVALID_ALL 6

struct sig_info_t
{
    int64_t error;
    int64_t code;
    int64_t status;
    int64_t pid;
    int64_t tid;
};

SYS_CALL(40, int, raise, int signum, sig_info_t *info)

struct sigtarget_t
{
    int64_t id;
    int64_t flags;
};

#define SIGTGT_PROC 1
#define SIGTGT_GROUP 2
typedef uint64_t sig_mask_t;

static inline void sig_mask_init(sig_mask_t &mask) { mask = 0; }

static inline void sig_mask_set(sig_mask_t &mask, int idx) { mask |= (1ul << idx); }

static inline void sig_mask_clear(sig_mask_t &mask, int idx) { mask &= ~(1ul << idx); }

static inline bool sig_mask_get(sig_mask_t mask, int idx) { return mask & (1ul << idx); }

SYS_CALL(41, int, sigsend, sigtarget_t *target, int signum, sig_info_t *info)
SYS_CALL(42, void, sigwait, int *num, sig_info_t *info)
SYS_CALL(43, int, sigmask, int opt, sig_mask_t *valid, sig_mask_t *block, sig_mask_t *ignore)

SYS_CALL(47, int, get_cpu_running)
SYS_CALL(48, void, setcpumask, uint64_t mask0, uint64_t mask1)
SYS_CALL(49, void, getcpumask, uint64_t *mask0, uint64_t *mask1)
SYS_CALL(50, bool, brk, uint64_t ptr)
SYS_CALL(51, uint64_t, sbrk, int64_t offset)

#define MMAP_READ 1
#define MMAP_WRITE 2
#define MMAP_EXEC 4
#define MMAP_FILE 8
#define MMAP_SHARED 16

SYS_CALL(52, void *, mmap, uint64_t start, fd_t fd, uint64_t offset, uint64_t len, uint64_t flags)
SYS_CALL(53, uint64_t, mumap, void *addr)
SYS_CALL(54, int64_t, create_msg_queue, uint64_t msg_count, uint64_t msg_bytes)
SYS_CALL(55, int64_t, write_msg_queue, int64_t key, uint64_t type, const void *buffer, uint64_t size, uint64_t flags)
SYS_CALL(56, int64_t, read_msg_queue, int64_t key, uint64_t type, void *buffer, uint64_t size, uint64_t flags)
SYS_CALL(57, void, close_msg_queue, int64_t key)

#define MSGQUEUE_FLAGS_NOBLOCK 1
#define MSGQUEUE_FLAGS_NOBLOCKOTHER 2

#define OK 0
#define EOF -1
#define ETIMEOUT -2
#define EINTR -3
#define EPARAM -4
#define EBUFFER -5
#define ESIZE -6
#define EPERMISSION -7
#define ERESOURCE_NOT_NULL -8
#define ENOEXIST -9
#define EINNER -10
#define EFAILED -11
#define ECONTI -12
