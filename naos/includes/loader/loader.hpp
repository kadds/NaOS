#pragma once
#include "common.hpp"
#include <new>
#include <utility>
typedef u16 io_port;

static inline u8 io_in8(io_port port)
{
    unsigned char ret = 0;
    __asm__ __volatile__("inb	%%dx,	%0	\n\t"
                         "mfence			\n\t"
                         : "=a"(ret)
                         : "d"(port)
                         : "memory");
    return ret;
}

static inline u16 io_in16(io_port port)
{
    unsigned short ret = 0;
    __asm__ __volatile__("inw	%%dx,	%0	\n\t"
                         "mfence			\n\t"
                         : "=a"(ret)
                         : "d"(port)
                         : "memory");
    return ret;
}

static inline u32 io_in32(io_port port)
{
    unsigned int ret = 0;
    __asm__ __volatile__("inl	%%dx,	%0	\n\t"
                         "mfence			\n\t"
                         : "=a"(ret)
                         : "d"(port)
                         : "memory");
    return ret;
}

static inline void io_out8(io_port port, u8 value)
{
    __asm__ __volatile__("outb	%0,	%%dx	\n\t"
                         "mfence			\n\t"
                         :
                         : "a"(value), "d"(port)
                         : "memory");
}

static inline void io_out16(io_port port, u16 value)
{
    __asm__ __volatile__("outw	%0,	%%dx	\n\t"
                         "mfence			\n\t"
                         :
                         : "a"(value), "d"(port)
                         : "memory");
}

static inline void io_out32(io_port port, u32 value)
{
    __asm__ __volatile__("outl	%0,	%%dx	\n\t"
                         "mfence			\n\t"
                         :
                         : "a"(value), "d"(port)
                         : "memory");
}
void *alloc(u32 size, u32 align);

template <typename T, typename... Args> T *New(Args &&... args)
{
    T *p = (T *)alloc(sizeof(T), alignof(T));
    return new (p) T(std::forward<Args>(args)...);
}

static inline void reverse_endian(u16 *dt)
{
    u8 low = *dt & 0xff;
    u8 high = *dt >> 8;
    *dt = (low << 8) | high;
}

static inline void reverse_endian(u32 *dt)
{
    reverse_endian((u16 *)dt);
    reverse_endian(((u16 *)dt) + 1);
}

ExportC void run_kernel(void *start_addr, kernel_start_args *);
ExportC bool _is_support_x64();