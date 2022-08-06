#pragma once
#include "common.hpp"
#include "freelibcxx/string.hpp"
namespace cmdline
{
struct space_t
{
    space_t(u64 space)
        : space(space)
    {
    }
    static space_t from_kib(u64 b) { return space_t(b << 10); }
    static space_t from_mib(u64 b) { return space_t(b << 20); }
    static space_t from_gib(u64 b) { return space_t(b << 30); }

    u64 space;
    u64 kib() { return space >> 10; }
    u64 mib() { return space >> 20; }
    u64 gib() { return space >> 30; }
};

bool early_get(const char *key, char *&out_buf, u64 &len);

space_t early_get_space(const char *key, space_t default_value);
bool early_get_bool(const char *key, bool default_value);

void init();

bool get(const char *key, freelibcxx::string &out);
i64 get_int(const char *key, i64 default_value);
u64 get_uint(const char *key, u64 default_value);
bool get_bool(const char *key, bool default_value);
space_t get_space(const char *key, space_t default_value);
}
