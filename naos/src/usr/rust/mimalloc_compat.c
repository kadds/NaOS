#include <stdio.h>
#include <errno.h>
#include <stddef.h>

extern int _s_yield(void);

FILE *stdin;
FILE *stdout;
FILE *stderr;

char *realpath(const char *path, char *resolved_path)
{
    (void)path;
    (void)resolved_path;
    errno = ENOTSUP;
    return NULL;
}

unsigned int sleep(unsigned int seconds)
{
    (void)seconds;
    (void)_s_yield();
    return 0;
}

int sched_yield(void)
{
    return _s_yield();
}

long strtol(const char *value, char **end, int base)
{
    if (end != NULL)
        *end = (char *)value;
    if (value == NULL || base != 10)
        return 0;

    int sign = 1;
    if (*value == '-') {
        sign = -1;
        value++;
    } else if (*value == '+') {
        value++;
    }
    long result = 0;
    const char *start = value;
    while (*value >= '0' && *value <= '9') {
        result = result * 10 + (*value - '0');
        value++;
    }
    if (end != NULL)
        *end = (char *)(value == start ? start : value);
    return sign * result;
}

int __popcountdi2(unsigned long long value)
{
#if defined(__POPCNT__)
    return __builtin_popcountll(value);
#else
    int count = 0;
    while (value != 0) {
        value &= value - 1;
        count++;
    }
    return count;
#endif
}
