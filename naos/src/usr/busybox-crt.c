#include <stddef.h>

void *__dso_handle;

extern void *memcpy(void *destination, const void *source, size_t size);

void *mempcpy(void *destination, const void *source, size_t size)
{
    return (char *)memcpy(destination, source, size) + size;
}
