#pragma once

extern "C" char _lib_sys_call;

#define SYS_CALL(index, ret, name, ...)                                                                                \
    extern "C" ret name(__VA_ARGS__);                                                                                  \
    __asm__(".globl " #name " \n\t "                                                                                   \
            ".type	" #name ",	@function \n\t" #name ": \n\t"                                                         \
            "movq $" #index ", %rax \n\t"                                                                              \
            "jmp _lib_sys_call \n\t ");

#define OPEN_READ 0
#define OPEN_WRITE 1

SYS_CALL(0, void, sys_none, void)
SYS_CALL(1, void, print, const char *arg, bool normal = false)

SYS_CALL(10, int, open, const char *path, unsigned long mode, unsigned long attr);
SYS_CALL(11, void, close, int fd);
SYS_CALL(12, unsigned long, write, int fd, const char *buffer, unsigned long len);
SYS_CALL(13, unsigned long, read, int fd, char *buffer, unsigned long max_len);
SYS_CALL(16, unsigned long, lseek, int fd, long offset, int mode);

SYS_CALL(30, void, exit, long ret)
SYS_CALL(31, void, sleep, unsigned long ms)
SYS_CALL(34, long, create_process, const char *filename, const char *args, unsigned long flags)
SYS_CALL(35, long, create_thread, void *entry, unsigned long arg, unsigned long flags)
SYS_CALL(36, int, detach, long tid)
SYS_CALL(37, int, join, long tid, long *ret)
SYS_CALL(38, long, wait_process, long pid, long *ret)
SYS_CALL(39, void, exit_thread, long ret, long state)

SYS_CALL(50, bool, brk, unsigned long ptr)
SYS_CALL(51, unsigned long, sbrk, long offset)
