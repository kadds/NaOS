#pragma once

extern "C" char _lib_sys_call;

#define SYS_CALL(index, ret, name, ...)                                                                                \
    extern "C" ret name(__VA_ARGS__);                                                                                  \
    __asm__(".globl " #name " \n\t "                                                                                   \
            ".type	" #name ",	@function \n\t" #name ": \n\t"                                                         \
            "movq $" #index ", %rax \n\t"                                                                              \
            "jmp _lib_sys_call \n\t ");

SYS_CALL(0, void, sys_none, void)
SYS_CALL(1, void, print, const char *arg, bool normal = false)
SYS_CALL(5, void, sleep, unsigned long ms)
