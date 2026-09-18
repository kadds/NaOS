#ifndef NAOS_MIMALLOC_UNISTD_H
#define NAOS_MIMALLOC_UNISTD_H

#include <stddef.h>

char *realpath(const char *path, char *resolved_path);
unsigned int sleep(unsigned int seconds);

#endif
