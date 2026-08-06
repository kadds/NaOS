#pragma once

// Kernel syscall results use Linux/POSIX errno numbers with a negative sign.
// User-space syscall wrappers convert negative results to errno.
#ifdef OK
#undef OK
#endif
#define OK 0

#ifdef EOF
#undef EOF
#endif
#define EOF -1

#ifdef EPERM
#undef EPERM
#endif
#define EPERM -1

#ifdef ENOENT
#undef ENOENT
#endif
#define ENOENT -2

#ifdef ECHILD
#undef ECHILD
#endif
#define ECHILD -10

#ifdef EINTR
#undef EINTR
#endif
#define EINTR -4

#ifdef EIO
#undef EIO
#endif
#define EIO -5

#ifdef ENXIO
#undef ENXIO
#endif
#define ENXIO -6

#ifdef ENOEXEC
#undef ENOEXEC
#endif
#define ENOEXEC -8

#ifdef EAGAIN
#undef EAGAIN
#endif
#define EAGAIN -11

#ifdef EACCES
#undef EACCES
#endif
#define EACCES -13

#ifdef EBADF
#undef EBADF
#endif
#define EBADF -9

#ifdef EFAULT
#undef EFAULT
#endif
#define EFAULT -14

#ifdef EBUSY
#undef EBUSY
#endif
#define EBUSY -16

#ifdef EEXIST
#undef EEXIST
#endif
#define EEXIST -17

#ifdef ENOMEM
#undef ENOMEM
#endif
#define ENOMEM -12

#ifdef EINVAL
#undef EINVAL
#endif
#define EINVAL -22

#ifdef ENOTTY
#undef ENOTTY
#endif
#define ENOTTY -25

#ifdef EOVERFLOW
#undef EOVERFLOW
#endif
#define EOVERFLOW -75

#ifdef ENOTSUP
#undef ENOTSUP
#endif
#define ENOTSUP -95

#ifdef ETIMEDOUT
#undef ETIMEDOUT
#endif
#define ETIMEDOUT -110

// Compatibility names used by existing kernel code. They intentionally alias
// standard errno values so there is one canonical error-number vocabulary.
#ifdef ETIMEOUT
#undef ETIMEOUT
#endif
#define ETIMEOUT ETIMEDOUT

#ifdef EPARAM
#undef EPARAM
#endif
#define EPARAM EINVAL

#ifdef EBUFFER
#undef EBUFFER
#endif
#define EBUFFER EFAULT

#ifdef ESIZE
#undef ESIZE
#endif
#define ESIZE EOVERFLOW

#ifdef EPERMISSION
#undef EPERMISSION
#endif
#define EPERMISSION EACCES

#ifdef ERESOURCE_NOT_NULL
#undef ERESOURCE_NOT_NULL
#endif
#define ERESOURCE_NOT_NULL EBUSY

#ifdef ENOEXIST
#undef ENOEXIST
#endif
#define ENOEXIST ENOENT

#ifdef EINNER
#undef EINNER
#endif
#define EINNER EIO

#ifdef EFAILED
#undef EFAILED
#endif
#define EFAILED EIO

#ifdef ECONTI
#undef ECONTI
#endif
#define ECONTI EAGAIN

#ifdef ENOTYPE
#undef ENOTYPE
#endif
#define ENOTYPE EINVAL
