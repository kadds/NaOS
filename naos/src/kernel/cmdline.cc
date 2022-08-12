#include "kernel/cmdline.hpp"
#include "common.hpp"
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/iterator.hpp"
#include "freelibcxx/string.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"

namespace cmdline
{
freelibcxx::hash_map<freelibcxx::string, freelibcxx::string> *cmdmap;

bool early_get(const char *key, char *&out_buf, u64 &len)
{
    int key_len = strlen(key);
    char *p = (char *)memory::pa2va(phy_addr_t::from(kernel_args->command_line));
    const char *pos = strstr(p, key);
    int idx = pos - p;
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
        pos = strstr(pos, key);
        idx = pos - p;
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
    auto val = freelibcxx::const_string_view(beg, len);

    auto iter = freelibcxx::find_if(val.begin(), val.end(), [](const char ch) { return !(ch >= '0' && ch <= '9'); });
    auto index = iter.get() - val.begin().get();
    auto numval = val.substr(0, index).to_uint64();
    if (!numval.has_value())
    {
        return default_value;
    }
    size_t num = numval.value();

    char c = val.last();

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
    cmdmap = memory::New<freelibcxx::hash_map<freelibcxx::string, freelibcxx::string>>(memory::KernelCommonAllocatorV,
                                                                                       memory::KernelCommonAllocatorV);
    trace::debug("cmdline address ", trace::hex(cmdline), ". ", cmdline);

    const freelibcxx::string ss(memory::KernelCommonAllocatorV, cmdline);
    freelibcxx::const_string_view sv = ss.view();
    const auto arr = sv.split(' ', memory::KernelCommonAllocatorV);
    for (auto item : arr)
    {
        freelibcxx::const_string_view views[2];
        int n = item.split_n<2>('=', views);
        if (n >= 1)
        {
            cmdmap->insert(views[0].to_string(memory::KernelCommonAllocatorV),
                           views[1].to_string(memory::KernelCommonAllocatorV));
        }
    }
}

freelibcxx::optional<freelibcxx::string> get(const char *key)
{
    return cmdmap->get(freelibcxx::string::from_cstr(key));
}

bool get_bool(const char *key, bool default_value)
{
    bool res = false;
    auto value = get(key);
    if (!value.has_value())
    {
        return default_value;
    }
    if (value.value() == "on" || value.value() == "true")
    {
        res = true;
    }
    return res;
}

i64 get_int(const char *key, i64 default_value)
{
    auto value = get(key);
    if (value.has_value())
    {
        return value.value().view().to_int64().value_or(default_value);
    }
    return default_value;
}

u64 get_uint(const char *key, u64 default_value)
{
    auto value = get(key);
    if (value.has_value())
    {
        return value.value().view().to_uint64().value_or(default_value);
    }
    return default_value;
}

space_t get_space(const char *key, space_t default_value)
{
    auto value = get(key);
    if (!value.has_value())
    {
        return default_value;
    }
    auto val = value.value().view();
    auto iter = freelibcxx::find_if(val.begin(), val.end(), [](const char ch) { return !(ch >= '0' && ch <= '9'); });
    auto index = iter.get() - val.begin().get();
    auto numval = val.substr(0, index).to_uint64();
    if (!numval.has_value())
    {
        return default_value;
    }
    size_t num = numval.value();

    char c = val.last();

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