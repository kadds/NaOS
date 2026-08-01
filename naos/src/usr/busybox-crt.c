#include <stddef.h>

void *__dso_handle;

extern void *memcpy(void *destination, const void *source, size_t size);
extern int *__errno_location(void);

void *mempcpy(void *destination, const void *source, size_t size)
{
    return (char *)memcpy(destination, source, size) + size;
}

int ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;
    *__errno_location() = 38; // ENOSYS
    return -1;
}
