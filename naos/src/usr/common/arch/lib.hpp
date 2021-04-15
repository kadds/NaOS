#pragma once

extern "C" char _lib_sys_call;

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

SYS_CALL(2, int, open, const char *path, unsigned long mode, unsigned long attr)
SYS_CALL(3, void, close, int fd);

#define RWFLAGS_NO_BLOCK 1
#define RWFLAGS_OVERRIDE 2

SYS_CALL(4, long, write, int fd, const char *buffer, unsigned long len, unsigned long flags)
SYS_CALL(5, long, read, int fd, char *buffer, unsigned long max_len, unsigned long flags)

#define LSEEK_MODE_CURRENT 0
#define LSEEK_MODE_BEGIN 1
#define LSEEK_MODE_END 2

SYS_CALL(6, long, pwrite, int fd, unsigned long offset, const char *buffer, unsigned long len, unsigned long flags)
SYS_CALL(7, long, pread, int fd, unsigned long offset, char *buffer, unsigned long max_len, unsigned long flags)

SYS_CALL(8, long, lseek, int fd, long offset, int mode)

SYS_CALL(9, long, select, unsigned long size, int *rfd, int *wfd, int *errfd, unsigned long flags)

SYS_CALL(10, long, get_pipe, int *fd1, int *fd2)
SYS_CALL(11, int, create_fifo, const char *path, unsigned long mode)

SYS_CALL(12, long, fcntl, int fd, unsigned int operator_type, unsigned int target, unsigned int attr, void *value,
         unsigned long size)

SYS_CALL(17, int, rename, const char *src, const char *target);
SYS_CALL(18, int, symbolink, const char *src, const char *target, unsigned long flags);

SYS_CALL(19, int, create, const char *filepath)

#define ACCESS_MODE_READ 1
#define ACCESS_MODE_WRITE 2
#define ACCESS_MODE_EXEC 4
#define ACCESS_MODE_EXIST 8

SYS_CALL(20, int, access, long mode)

SYS_CALL(21, int, mkdir, const char *path)
SYS_CALL(22, int, rmdir, const char *path)
SYS_CALL(23, int, chdir, const char *new_workpath)
SYS_CALL(24, long, current_dir, char *path, unsigned long max_len)
SYS_CALL(25, int, chroot, const char *path)
SYS_CALL(26, int, link, const char *src, const char *target)
SYS_CALL(27, int, unlink, const char *target)

SYS_CALL(28, int, mount, const char *dev, const char *mount_point, const char *fs_type, unsigned long flags,
         const char *data, unsigned long size)
SYS_CALL(29, int, umount, const char *mount_point)

SYS_CALL(30, void, exit, long ret)
SYS_CALL(31, void, sleep, unsigned long ms)
SYS_CALL(32, long, current_pid)
SYS_CALL(33, long, current_tid)

#define CP_FLAG_NORETURN 1
#define CP_FLAG_BINARY 2
#define CP_FLAG_SHARED_FILE 8
#define CP_FLAG_SHARED_NOROOT 16
#define CP_FLAG_SHARED_WORK_DIR 32

SYS_CALL(34, long, create_process, const char *filename, char *const args[], unsigned long flags)

#define CT_FLAG_IMMEDIATELY 1
#define CT_FLAG_NORETURN 4

SYS_CALL(35, long, create_thread, void *entry, unsigned long arg, unsigned long flags)
SYS_CALL(36, int, detach, long tid)
SYS_CALL(37, int, join, long tid, long *ret)
SYS_CALL(38, long, wait_process, long pid, long *ret)
SYS_CALL(39, [[noreturn]] void, exit_thread, long ret)

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
    long error;
    long code;
    long status;
    long pid;
    long tid;
};

SYS_CALL(40, int, raise, int signum, sig_info_t *info)

struct sigtarget_t
{
    long id;
    long flags;
};

#define SIGTGT_PROC 1
#define SIGTGT_GROUP 2
typedef unsigned long sig_mask_t;

static inline void sig_mask_init(sig_mask_t &mask) { mask = 0; }

static inline void sig_mask_set(sig_mask_t &mask, int idx) { mask |= (1ul << idx); }

static inline void sig_mask_clear(sig_mask_t &mask, int idx) { mask &= ~(1ul << idx); }

static inline bool sig_mask_get(sig_mask_t mask, int idx) { return mask & (1ul << idx); }

SYS_CALL(41, int, sigsend, sigtarget_t *target, int signum, sig_info_t *info)
SYS_CALL(42, void, sigwait, int *num, sig_info_t *info)
SYS_CALL(43, int, sigmask, int opt, sig_mask_t *valid, sig_mask_t *block, sig_mask_t *ignore)

SYS_CALL(47, int, get_cpu_running)
SYS_CALL(48, void, setcpumask, unsigned long mask0, unsigned long mask1)
SYS_CALL(49, void, getcpumask, unsigned long *mask0, unsigned long *mask1)
SYS_CALL(50, bool, brk, unsigned long ptr)
SYS_CALL(51, unsigned long, sbrk, long offset)

#define MMAP_READ 1
#define MMAP_WRITE 2
#define MMAP_EXEC 4
#define MMAP_FILE 8
#define MMAP_SHARED 16

SYS_CALL(52, void *, mmap, unsigned long start, int fd, unsigned long offset, unsigned long len, unsigned long flags)
SYS_CALL(53, unsigned long, mumap, void *addr)
SYS_CALL(54, long, create_msg_queue, unsigned long msg_count, unsigned long msg_bytes)
SYS_CALL(55, long, write_msg_queue, long key, unsigned long type, const void *buffer, unsigned long size,
         unsigned long flags)
SYS_CALL(56, long, read_msg_queue, long key, unsigned long type, void *buffer, unsigned long size, unsigned long flags)
SYS_CALL(57, void, close_msg_queue, long key)

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