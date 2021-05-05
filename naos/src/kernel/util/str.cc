#include "kernel/util/str.hpp"
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

void string::append(const string &rhs) { append_buffer(rhs.data(), rhs.size()); }

void string::append_buffer(const char *buf, u64 length)
{
    cassert(!is_shared());
    u64 count = get_count();
    u64 new_count = count + length;
    char *old_buf = nullptr;
    memory::IAllocator *allocator = nullptr;
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
            memcopy(old_buf + count, buf, length);
            old_buf[new_count] = 0;
            stack.set_count(new_count);
            return;
        }
    }
    else if (head.get_cap() > new_count)
    {
        memcopy(head.get_buffer() + count, buf, length);
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
    memcopy(new_buf + count, buf, length);
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

void string::push_back(char ch) { append_buffer(&ch, 1); }

char string::pop_back()
{
    const char *b = data();
    u64 length = size();
    char c = b[length - 1];
    remove_at(length - 1, length);
    return c;
}

void string::remove_at(u64 index, u64 end_index)
{
    cassert(!is_shared());
    u64 length = end_index - index;
    u64 count = get_count();
    cassert(index < end_index && end_index <= count);
    u64 new_count = count - length;
    char *old_buf = nullptr;
    memory::IAllocator *allocator = nullptr;
    if (!is_sso())
    {
        // head
        old_buf = head.get_buffer();
        if (new_count < stack.get_cap())
        {
            allocator = head.get_allocator();
            stack.set_allocator(allocator);
            memcopy(stack.get_buffer(), old_buf, index);
            memcopy(stack.get_buffer() + index, old_buf + end_index, new_count - index);
            stack.get_buffer()[new_count] = 0;
            allocator->deallocate(old_buf);
            stack.set_count(new_count);
        }
        else
        {
            memcopy(old_buf + index, old_buf + end_index, new_count - index);
            old_buf[new_count] = 0;
            head.set_count(new_count);
        }
    }
    else
    {
        old_buf = stack.get_buffer();
        memcopy(old_buf + index, old_buf + end_index, length);
        old_buf[new_count] = 0;
        stack.set_count(new_count);
    }
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
