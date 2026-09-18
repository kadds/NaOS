#ifndef NAOS_MIMALLOC_STDLIB_H
#define NAOS_MIMALLOC_STDLIB_H

#include <stddef.h>

void abort(void) __attribute__((noreturn));
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
long strtol(const char *value, char **end, int base);

#endif
