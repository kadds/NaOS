#ifndef NAOS_MIMALLOC_ERRNO_H
#define NAOS_MIMALLOC_ERRNO_H

extern int naos_mimalloc_errno;
#define errno naos_mimalloc_errno

#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EINVAL 22
#define ENOSYS 38
#define ENOTSUP 95
#define EIO 5
#define ENOENT 2
#define ENOTDIR 20
#define ENAMETOOLONG 36
#define EOVERFLOW 75

#endif
