.text

.macro naos_syscall name, number
.globl \name
.type \name, @function
\name:
    movq $\number, %rax
    pushq %r10
    pushq %r11
    pushq %r12
    movq %rcx, %r12
    movq %rsp, %r10
    syscall
    popq %r12
    popq %r11
    popq %r10
    retq
.size \name, .-\name
.endm

naos_syscall _s_log, 1
naos_syscall _s_exit, 4
naos_syscall _na_handle_close, 17
naos_syscall _na_channel_create, 18
naos_syscall _na_channel_send, 19
naos_syscall _na_channel_receive, 20
naos_syscall _na_handle_wait_many, 22
naos_syscall _na_handle_duplicate, 23
naos_syscall _na_handle_get_info, 25
naos_syscall _na_bootstrap, 34
