#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <naos/syscall.h>

extern "C" uint64_t naos_rust_tls_probe(uint64_t value);
extern "C" int __cxa_thread_atexit_impl(void (*function)(void *), void *argument, void *dso_symbol);

extern "C" int __cxa_thread_atexit(void (*function)(void *), void *argument, void *dso_symbol)
{
    return __cxa_thread_atexit_impl(function, argument, dso_symbol);
}

namespace
{
volatile uint32_t child_destructors = 0;
volatile uint32_t detached_completed = 0;

struct tls_object
{
    uint64_t value;

    ~tls_object()
    {
        __atomic_fetch_add(&child_destructors, 1, __ATOMIC_RELEASE);
    }
};

thread_local uint64_t compiler_tls_value = 0x11;
thread_local tls_object destructor_tls{0x11};

void *joinable_worker(void *)
{
    if (compiler_tls_value != 0x11 || destructor_tls.value != 0x11 || naos_rust_tls_probe(0x22) != 0x11)
        return reinterpret_cast<void *>(1);
    compiler_tls_value = 0x22;
    destructor_tls.value = 0x22;
    return reinterpret_cast<void *>(0x22);
}

void *detached_worker(void *)
{
    if (compiler_tls_value != 0x11 || destructor_tls.value != 0x11 || naos_rust_tls_probe(0x33) != 0x11)
        return reinterpret_cast<void *>(2);
    compiler_tls_value = 0x33;
    destructor_tls.value = 0x33;
    __atomic_store_n(&detached_completed, 1, __ATOMIC_RELEASE);
    return reinterpret_cast<void *>(0x33);
}
} // namespace

void mlibc_tls_smoke_log(const char *message)
{
    _s_log(message);
    fputs(message, stdout);
    fflush(stdout);
}

extern "C" int run_mlibc_tls_smoke()
{
    if (naos_rust_tls_probe(0x11) != 0x11)
        return 8;

    for (int index = 0; index < 4; ++index)
    {
        pthread_t thread{};
        if (pthread_create(&thread, nullptr, joinable_worker, nullptr) != 0)
            return 1;

        void *result = nullptr;
        if (pthread_join(thread, &result) != 0 || result != reinterpret_cast<void *>(0x22) ||
            compiler_tls_value != 0x11 ||
            __atomic_load_n(&child_destructors, __ATOMIC_ACQUIRE) != static_cast<uint32_t>(index + 1))
            return 2;
    }

    pthread_t detached{};
    if (pthread_create(&detached, nullptr, detached_worker, nullptr) != 0 || pthread_detach(detached) != 0)
        return 3;
    while (__atomic_load_n(&detached_completed, __ATOMIC_ACQUIRE) == 0)
        __asm__ volatile("pause");
    while (__atomic_load_n(&child_destructors, __ATOMIC_ACQUIRE) != 5)
        __asm__ volatile("pause");

    pthread_t exited_joinable{};
    if (pthread_create(&exited_joinable, nullptr, joinable_worker, nullptr) != 0)
        return 5;
    while (__atomic_load_n(&child_destructors, __ATOMIC_ACQUIRE) != 6)
        __asm__ volatile("pause");
    if (pthread_detach(exited_joinable) != 0)
        return 6;

    if (compiler_tls_value != 0x11 || destructor_tls.value != 0x11 || naos_rust_tls_probe(0x11) != 0x11)
        return 7;
    mlibc_tls_smoke_log("mlibc-tls: shared Rust/C++ PT_TLS, TCB, destructor, join, detach ready\n");
    return 0;
}
