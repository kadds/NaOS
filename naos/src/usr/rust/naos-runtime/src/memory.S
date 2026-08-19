.text

.globl memcpy
.type memcpy, @function
memcpy:
    movq %rdi, %rax
    movq %rdx, %rcx
    rep movsb
    retq
.size memcpy, .-memcpy

.globl memmove
.type memmove, @function
memmove:
    movq %rdi, %rax
    cmpq %rdi, %rsi
    jae 1f
    leaq -1(%rdi,%rdx), %rdi
    leaq -1(%rsi,%rdx), %rsi
    movq %rdx, %rcx
    std
    rep movsb
    cld
    retq
1:
    movq %rdx, %rcx
    rep movsb
    retq
.size memmove, .-memmove

.globl memset
.type memset, @function
memset:
    movq %rdi, %rax
    movq %rdx, %rcx
    movb %sil, %al
    rep stosb
    movq %rdi, %rax
    subq %rdx, %rax
    retq
.size memset, .-memset

.globl memcmp
.type memcmp, @function
memcmp:
    testq %rdx, %rdx
    je 2f
    movq %rdx, %rcx
    repe cmpsb
    je 2f
    movzbl -1(%rdi), %eax
    movzbl -1(%rsi), %edx
    subl %edx, %eax
    retq
2:
    xorl %eax, %eax
    retq
.size memcmp, .-memcmp
