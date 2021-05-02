#include "kernel/util/str.hpp"
#include "kernel/util/formatter.hpp"
namespace util
{
int strcmp(const char *str1, const char *str2)
{
    while (1)
    {
        char s1, s2;
        s1 = *str1++;
        s2 = *str2++;
        if (s1 != s2)
            return s1 < s2 ? -1 : 1;
        if (!s1)
            break;
    }
    return 0;
}

i64 strcopy(char *dst, const char *src, i64 max_len)
{
    i64 i = 0;
    do
    {
        *dst++ = *src++;
    } while (*src != 0 && i++ < max_len);

    return i - 1;
}

i64 strcopy(char *dst, const char *src)
{
    const char *s = src;
    do
    {
        *dst++ = *src++;
    } while (*src != 0);

    return src - s - 1;
}

i64 strlen(const char *str)
{
    i64 i = 0;
    while (*str++ != 0)
        i++;
    return i++;
}

i64 strfind(const char *str, const char *pat)
{
    const char *beg = str;
    const char *sbeg = pat;
    while (*beg != 0)
    {
        const char *p = beg;
        while (*sbeg == *p && *sbeg != 0)
        {
            p++;
            sbeg++;
        }
        if (*sbeg == 0)
        {
            // get it
            return beg - str;
        }
        beg++;
    }
    return -1;
}

string string_view::to_string(memory::IAllocator *allocator) { return string(allocator, ptr, len); }

string::string(memory::IAllocator *allocator)
{
    head.init();
    stack.set_count(0);
    stack.set_allocator(allocator);
    stack.get_buffer()[0] = 0;
}

string &string::operator=(const string &rhs)
{
    if (this == &rhs)
        return *this;
    free();
    copy(rhs);
    return *this;
}

string &string::operator=(string &&rhs)
{
    if (this == &rhs)
        return *this;
    free();
    move(std::move(rhs));
    return *this;
}

array<string_view> string::split(char c, memory::IAllocator *vec_allocator)
{
    array<string_view> vec(vec_allocator);
    char *p;
    u64 count;
    if (unlikely(this->is_sso()))
    {
        p = stack.get_buffer();
        count = stack.get_count();
    }
    else
    {
        p = head.get_buffer();
        count = head.get_count();
    }
    char *prev = p;
    for (u64 i = 0; i < count; i++)
    {
        if (*p == c)
        {
            if (prev < p)
            {
                vec.push_back(std::move(string_view(prev, p - prev)));
            }
            prev = p + 1;
        }
        p++;
    }
    if (prev < p)
    {
        vec.push_back(std::move(string_view(prev, p - prev)));
    }
    return vec;
}

bool string::to_int(i64 &out) const
{
    const char *beg = data();
    const char *end = beg + size();
    return formatter::str2int(beg, end, out) != beg;
}

bool string::to_uint(u64 &out) const
{
    const char *beg = data();
    const char *end = beg + size();
    return formatter::str2uint(beg, end, out) != beg;
}

bool string::to_int_ext(i64 &out, string_view &last) const
{
    const char *beg = data();
    const char *end = beg + size();
    beg = formatter::str2int(beg, end, out);
    // last = string_view(beg, beg - data());
    return beg != data();
}

bool string::to_uint_ext(u64 &out, string_view &last) const
{
    const char *beg = data();
    const char *end = beg + size();
    beg = formatter::str2uint(beg, end, out);
    // last = string_view(beg, beg - data());
    return beg != data();
}

void string::append(const string &rhs)
{
    cassert(!is_shared());
    u64 count = get_count();
    u64 rc = rhs.get_count();
    u64 new_count = count + rc;
    char *old_buf = nullptr;
    memory::IAllocator *allocator = nullptr;
    const char *buf = rhs.data();
    bool is_stack = false;
    if (likely(is_sso()))
    {
        old_buf = stack.get_buffer();
        if (new_count >= stack.get_cap())
        {
            // try allocate memory
            // switch to head
            is_stack = true;
            allocator = stack.get_allocator();
        }
        else
        {
            memcopy(old_buf + count, buf, rc);
            stack.get_buffer()[new_count] = 0;
            stack.set_count(new_count);
            return;
        }
    }
    else if (head.get_cap() > new_count)
    {
        memcopy(head.get_buffer() + count, buf, rc);
        head.get_buffer()[new_count] = 0;
        head.set_count(new_count);
        return;
    }
    else
    {
        old_buf = head.get_buffer();
        allocator = head.get_allocator();
    }
    // append to head
    u64 cap = select_capacity(new_count + 1);
    char *new_buf = reinterpret_cast<char *>(allocator->allocate(cap, 8));
    memcopy(new_buf, old_buf, count);
    memcopy(new_buf + count, buf, rc);
    new_buf[new_count] = 0;
    if (!is_stack)
    {
        allocator->deallocate(old_buf);
    }
    head.set_allocator(allocator);
    head.set_cap(cap);
    head.set_count(new_count);
    head.set_buffer(new_buf);
}
bool string::operator==(const string &rhs) const
{
    if (unlikely(&rhs == this))
        return true;
    u64 lc = get_count();
    u64 rc = rhs.get_count();
    if (lc != rc)
    {
        return false;
    }
    const char *l = data();
    const char *r = rhs.data();
    return memcmp(l, r, lc) == 0;
}

u64 string::select_capacity(u64 capacity)
{
    u64 new_cap = capacity;
    return new_cap;
}
void string::free()
{
    if (likely(is_sso()))
    {
        stack.set_count(0);
        stack.get_buffer()[0] = 0;
    }
    else if (head.get_buffer() != nullptr)
    {
        auto allocator = head.get_allocator();
        if (allocator != nullptr)
        {
            allocator->deallocate(head.get_buffer());
        }
        head.set_buffer(nullptr);
        head.set_count(0);
        head.set_cap(0);
    }
}
void string::copy(const string &rhs)
{
    if (likely(rhs.is_sso()))
    {
        this->stack = rhs.stack;
    }
    else
    {
        auto a = rhs.head.get_allocator();
        u64 size = rhs.head.get_count();
        u64 cap = rhs.head.get_cap();
        head.set_allocator(a);
        head.set_count(size);
        if (unlikely(a == nullptr)) // shared string
        {
            head.set_buffer(const_cast<char *>(rhs.head.get_buffer()));
        }
        else
        {
            cap = select_capacity(cap);
            const char *s = rhs.head.get_buffer();
            char *p = reinterpret_cast<char *>(a->allocate(cap, 8));
            memcopy(p, s, size);
            p[size] = 0;
        }
        head.set_cap(cap);
    }
}

void string::move(string &&rhs)
{
    if (likely(rhs.is_sso()))
    {
        this->stack = rhs.stack;
        rhs.stack.set_count(0);
        rhs.stack.get_buffer()[0] = 0; // end \0
    }
    else
    {
        this->head = rhs.head;
        rhs.head.set_count(0);
        rhs.head.set_buffer(nullptr);
        rhs.head.set_cap(0);
    }
}
void string::init(memory::IAllocator *allocator, const char *str, i64 len)
{
    head.init();
    if (len < 0)
        len = (i64)strlen(str);
    if ((u64)len >= stack.get_cap())
    {
        head.set_allocator(allocator);
        // go to head
        u64 cap = select_capacity(len + 1);
        char *buf = reinterpret_cast<char *>(allocator->allocate(cap, 8));
        memcopy(buf, str, len);
        buf[len] = 0; // end \0
        head.set_count(len);
        head.set_cap(cap);
        head.set_buffer(buf);
    }
    else
    {
        stack.set_allocator(allocator);
        stack.set_count(len);
        memcopy(stack.get_buffer(), str, len);
        stack.get_buffer()[len] = 0;
    }
}

void string::init_lit(const char *str)
{
    head.init();
    u64 l = strlen(str);
    head.set_allocator(nullptr);
    head.set_count(l);
    head.set_cap(l + 1);
    // readonly string
    head.set_buffer(const_cast<char *>(str));
}

} // namespace util
