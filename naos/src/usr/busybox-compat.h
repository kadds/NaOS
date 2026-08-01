#pragma once

#ifndef __ASSEMBLER__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *mempcpy(void *destination, const void *source, size_t size);

#if !defined(__MLIBC_LINUX_OPTION) || !__MLIBC_LINUX_OPTION
int sched_getaffinity(int pid, size_t cpusetsize, void *mask);
#endif

#ifdef __cplusplus
}
#endif

#endif
