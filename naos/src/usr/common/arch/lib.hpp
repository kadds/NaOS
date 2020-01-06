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

SYS_CALL(10, int, open, const char *path, unsigned long mode, unsigned long attr)
SYS_CALL(11, void, close, int fd);
#define EOF -1
#define RWFLAGS_NO_BLOCK 1
#define RWFLAGS_OVERRIDE 2

SYS_CALL(12, unsigned long, write, int fd, const char *buffer, unsigned long len, unsigned long flags)
SYS_CALL(13, unsigned long, read, int fd, char *buffer, unsigned long max_len, unsigned long flags)

#define LSEEK_MODE_CURRENT 0
#define LSEEK_MODE_BEGIN 1
#define LSEEK_MODE_END 2

SYS_CALL(16, unsigned long, lseek, int fd, long offset, int mode)

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
SYS_CALL(24, unsigned long, current_dir, char *path, unsigned long max_len)
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

SYS_CALL(34, long, create_process, const char *filename, const char *args, unsigned long flags)

#define CT_FLAG_IMMEDIATELY 1
#define CT_FLAG_NORETURN 4

SYS_CALL(35, long, create_thread, void *entry, unsigned long arg, unsigned long flags)
SYS_CALL(36, int, detach, long tid)
SYS_CALL(37, int, join, long tid, long *ret)
SYS_CALL(38, long, wait_process, long pid, long *ret)
SYS_CALL(39, void, exit_thread, long ret)

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

SYS_CALL(40, int, sigaction, int signum, void (*handler)(int signum, long error, long code, long status),
         unsigned long long mask, int flags)
SYS_CALL(41, int, raise, int signum, int error, int code, int status)
SYS_CALL(42, int, sigsend, int tid, int signum, int error, int code, int status)
SYS_CALL(44, void, sigreturn, int code)

SYS_CALL(50, bool, brk, unsigned long ptr)
SYS_CALL(51, unsigned long, sbrk, long offset)

#define MMAP_READ 1
#define MMAP_WRITE 2
#define MMAP_EXEC 4
#define MMAP_FILE 8
#define MMAP_SHARED 16

SYS_CALL(52, void *, mmap, unsigned long start, int fd, unsigned long offset, unsigned long len, unsigned long flags)
SYS_CALL(53, unsigned long, mumap, void *addr)
