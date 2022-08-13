#pragma once

#ifdef OK
#undef OK
#endif
#define OK 0

#ifdef EOF
#undef EOF
#endif
#define EOF -1

#ifdef ETIMEOUT
#undef ETIMEOUT
#endif
#define ETIMEOUT -2

#ifdef EINTR
#undef EINTR
#endif
#define EINTR -3

#ifdef EPARAM
#undef EPARAM
#endif
#define EPARAM -4

#ifdef EBUFFER
#undef EBUFFER
#endif
#define EBUFFER -5

#ifdef ESIZE
#undef ESIZE
#endif
#define ESIZE -6

#ifdef EPERMISSION
#undef EPERMISION
#endif
#define EPERMISSION -7

#ifdef ERESOURCE_NOT_NULL
#undef ERESOURCE_NOT_NULL
#endif
#define ERESOURCE_NOT_NULL -8

#ifdef ENOEXIST
#undef ENOEXIST
#endif
#define ENOEXIST -9

#ifdef EINNER
#undef EINNER
#endif
#define EINNER -10

#ifdef EFAILED
#undef EFAILED
#endif
#define EFAILED -11

#ifdef EOUNTI
#undef ECOUNI
#endif
#define ECONTI -12ul

#ifdef ENOEXEC
#undef ENOEXEC
#endif
#define NOEXEC -13

#ifdef ENOTTYPE
#undef ENOTYPE
#endif
#define ENOTYPE -14

#ifdef ENOTTY
#undef ENOTTY
#endif
#define ENOTTY 25