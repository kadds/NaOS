#include "kernel/cmdline.hpp"
#include "common.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/hash_map.hpp"
#include "kernel/util/memory.hpp"
#include "kernel/util/str.hpp"

namespace cmdline
{
using namespace util;
hash_map<string, string> *cmdmap;

bool early_get(const char *key, char *&out_buf, u64 &len)
{
    i64 key_len = util::strlen(key);
    char *p = (char *)memory::pa2va(phy_addr_t::from(kernel_args->command_line));
    int idx = strfind(p, key);
    while (idx >= 0)
    {
        char *beg = p + idx + key_len;
        if (*beg != 0 && *beg == '=')
        {
            char *s = beg + 1;
            while (*s != 0 && *s != ' ')
            {
                s++;
            }
            len = s - beg - 1;
            out_buf = beg + 1;
            return true;
        }
        idx = strfind(p + idx, key);
    };
    len = 0;
    return false;
}

space_t early_get_space(const char *key, space_t default_value)
{
    u64 len = 0;
    char *beg;

    if (!early_get(key, beg, len) || len == 0)
    {
        return default_value;
    }
    const char *item_end = beg + len;
    u64 num;
    const char *last = formatter::str2uint(beg, item_end, num);
    if (last == beg)
    {
        // Cast fail
        return default_value;
    }
    space_t ret(num);
    if (last < item_end)
    {
        char c = *last;
        if (c < 'a')
        {
            c += 'a' - 'A';
        }
        if (c == 'k')
        {
            ret = space_t::from_kib(num);
        }
        else if (c == 'm')
        {
            ret = space_t::from_mib(num);
        }
        else if (c == 'g')
        {
            ret = space_t::from_gib(num);
        }
    }
    return ret;
}

bool early_get_bool(const char *key, bool default_value)
{
    u64 len = 0;
    char *beg;
    if (!early_get(key, beg, len) || len == 0)
    {
        return default_value;
    }
    bool ret = false;
    if (len == 2)
    {
        if (beg[0] == 'o' && beg[1] == 'n')
        {
            ret = true;
        }
    }
    else if (len == 4)
    {
        if (beg[0] == 't' && beg[1] == 'r' && beg[2] == 'u' && beg[3] == 'e')
        {
            ret = true;
        }
    }
    return ret;
}

void init()
{
    char *cmdline = (char *)memory::pa2va(phy_addr_t::from(kernel_args->command_line));
    cmdmap = memory::New<hash_map<string, string>>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    trace::debug("cmdline address ", reinterpret_cast<coaddr_t>(cmdline), ". ", cmdline);

    const string ss(cmdline);
    const_string_view sv = ss.view();
    const auto arr = sv.split(' ', memory::KernelCommonAllocatorV);
    for (auto item : arr)
    {
        auto iter = find(item.begin(), item.end(), '=');
        const_string_view key, value;
        item.split2(key, value, iter);
        cmdmap->insert(key.to_string(memory::KernelCommonAllocatorV), value.to_string(memory::KernelCommonAllocatorV));
    }
}

bool get(const char *key, string &out) { return cmdmap->get(key, out); }

bool get_bool(const char *key, bool default_value)
{
    string value("");
    bool res = false;
    if (!get(key, value))
    {
        return default_value;
    }
    if (value == "on" || value == "true")
    {
        res = true;
    }
    return res;
}

i64 get_int(const char *key, i64 default_value)
{
    string value("");
    i64 res;
    if (!get(key, value) || !value.view().to_int(res))
    {
        return default_value;
    }
    return res;
}

u64 get_uint(const char *key, u64 default_value)
{
    string value("");
    u64 res;
    if (!get(key, value) || !value.view().to_uint(res))
    {
        return default_value;
    }
    return res;
}

space_t get_space(const char *key, space_t default_value)
{
    string value("");
    u64 num;
    string_view last;
    if (!get(key, value) || !value.view().to_uint_ext(num, last))
    {
        return default_value;
    }
    char c = *last.begin();

    space_t ret = default_value;
    if (c < 'a')
    {
        c += 'a' - 'A';
    }
    if (c == 'k')
    {
        ret = space_t::from_kib(num);
    }
    else if (c == 'm')
    {
        ret = space_t::from_mib(num);
    }
    else if (c == 'g')
    {
        ret = space_t::from_gib(num);
    }
    return ret;
}

} // namespace cmdline