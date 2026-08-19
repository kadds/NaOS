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
naos_syscall _s_clock, 2
naos_syscall _s_exit, 4
naos_syscall _s_exit_thread, 5
naos_syscall _s_sleep, 6
naos_syscall _s_current_pid, 7
naos_syscall _s_current_tid, 8
naos_syscall _s_futex, 3
naos_syscall _s_tcb_set, 11
naos_syscall _s_clone, 13
naos_syscall _s_yield, 14
naos_syscall _na_handle_close, 17
naos_syscall _na_channel_create, 18
naos_syscall _na_channel_send, 19
naos_syscall _na_channel_receive, 20
naos_syscall _na_channel_discard, 21
naos_syscall _na_handle_wait_many, 22
naos_syscall _na_handle_duplicate, 23
naos_syscall _na_handle_restrict, 24
naos_syscall _na_handle_get_info, 25
naos_syscall _na_protocol_descriptor_create, 26
naos_syscall _na_protocol_endpoint_create, 27
naos_syscall _na_invoke_submit, 28
naos_syscall _na_invoke_oneway, 29
naos_syscall _na_invocation_cancel, 30
naos_syscall _na_invocation_take_result, 31
naos_syscall _na_responder_reply, 32
naos_syscall _na_responder_fail, 33
naos_syscall _na_bootstrap, 34
naos_syscall _na_memory_map, 36
naos_syscall _na_memory_unmap, 37
