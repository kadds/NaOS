#pragma once
#include "array.hpp"
#include "assert.hpp"
#include "common.hpp"
#include "hash.hpp"
#include "kernel/mm/allocator.hpp"
#include "memory.hpp"
#include <utility>

namespace util
{

int strcmp(const char *str1, const char *str2);

///\brief copy src to dst (include \\0) same as 'strcopy(char *, const char *)'
i64 strcopy(char *dst, const char *src, i64 max_len);

///\brief copy src to dst (include \\0)
///\param dst copy to
///\param src copy from
///\return copy char count (not include \\0)
i64 strcopy(char *dst, const char *src);

///\brief get string length (not include \\0)
i64 strlen(const char *str);

///\brief kernel string type, use it everywhere
///
class string
{
  public:
    class iterator;

    string(const string &rhs) { copy(rhs); }

    string(string &&rhs) { move(std::move(rhs)); }

    ///\brief init empty string ""
    string(memory::IAllocator *allocator)
    {
        head.init();
        stack.set_allocator(allocator);
        stack.set_count(0);
        stack.get_buffer()[0] = 0;
    }

    ///\brief init from char array
    /// no_shared
    string(memory::IAllocator *allocator, const char *str, i64 len = -1) { init(allocator, str, len); }

    ///\brief init from char array lit
    /// readonly & shared
    string(const char *str) { init_lit(str); }

    ~string() { free(); }

    string &operator=(const string &rhs)
    {
        if (this == &rhs)
            return *this;
        free();
        copy(rhs);
        return *this;
    }

    string &operator=(string &&rhs)
    {
        if (this == &rhs)
            return *this;
        free();
        move(std::move(rhs));
        return *this;
    }

    class string_view
    {
      public:
        string_view(char *ptr, u64 len)
            : ptr(ptr)
            , len(len)
        {
        }
        string to_string(memory::IAllocator *allocator) { return string(allocator, ptr, len); }

      private:
        char *ptr;
        u64 len;
    };

    array<string_view> split(char c)
    {
        array<string_view> vec(head.get_allocator());
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
                    vec.push_back(std::move(string_view(prev, p - prev - 1)));
                }
                prev = p + 1;
            }
            p++;
        }
        if (prev < p)
        {
            vec.push_back(std::move(string_view(prev, p - prev - 1)));
        }
        return vec;
    }

    u64 size() const { return get_count(); }

    u64 capacity() const
    {
        if (likely(is_sso()))
        {
            return stack.get_cap();
        }
        else
        {
            return head.get_cap();
        }
    }

    u64 hash() const { return murmur_hash2_64(data(), get_count(), 0); }

    // iterator begin() const { return iterator(buffer); }

    // iterator end() const { return iterator(buffer + count); }

    bool is_shared() const
    {
        if (likely(is_sso()))
        {
            return false;
        }
        else
        {
            return head.get_allocator() == nullptr;
        }
    }

    char *data()
    {
        if (likely(is_sso()))
        {
            return stack.get_buffer();
        }
        else
        {
            return head.get_buffer();
        }
    }

    const char *data() const
    {
        if (likely(is_sso()))
        {
            return stack.get_buffer();
        }
        else
        {
            return head.get_buffer();
        }
    }

    void append(const string &rhs)
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

    char at(u64 index) const
    {
        cassert(index < get_count());
        return data()[index];
    }

    string &operator+=(const string &rhs)
    {
        append(rhs);
        return *this;
    }

    bool operator==(const string &rhs) const
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

    bool operator!=(const string &rhs) const { return !operator==(rhs); }

  private:
    // littel endian machine
    //      0x0
    // |-----------------|---------------|
    // |  count(63)      |  char(64) 8   |
    // | flag_shared(1)  |               |
    // |-----------------|---------------|
    // | buffer(64)     |  char(64) 8    |
    // |----------------|----------------|
    // | cap(63)        |  char(56) 7    |
    // | none(1)        |   count(5)     |
    // |----------------|----------------|
    // |  flag_type(1)0 |  flag_type(1)1 |
    // |  allocator(63) |  allocator(63) |
    // |----------------|----------------|
    // 0x1F

    struct head_t
    {
        u64 count;
        char *buffer;
        u64 cap;
        memory::IAllocator *allocator;
        u64 get_count() const { return count & ((1UL << 63) - 1); }
        void set_count(u64 c) { count = (c & ((1UL << 63) - 1)) | (count & (1UL << 63)); };
        u64 get_cap() const { return cap & ((1UL << 63) - 1); }
        void set_cap(u64 c) { cap = (c & ((1UL << 63) - 1)) | (cap & (1UL << 63)); }
        char *get_buffer() { return buffer; }
        const char *get_buffer() const { return buffer; }
        void set_buffer(char *b) { buffer = b; }

        memory::IAllocator *get_allocator() const { return allocator; }
        void set_allocator(memory::IAllocator *alc)
        {
            cassert((reinterpret_cast<u64>(alc) & 0x1) == 0); // bit 0
            allocator = alc;
        }
        void init()
        {
            count = 0;
            buffer = nullptr;
            cap = 0;
            allocator = 0;
        }
    };

    struct stack_t
    {
        byte data[24];
        memory::IAllocator *allocator;

      public:
        u64 get_count() const { return get_cap() - static_cast<u64>(data[16]); }
        void set_count(u64 c) { data[16] = static_cast<byte>(get_cap() - c); }
        u64 get_cap() const { return 23; }
        char *get_buffer() { return reinterpret_cast<char *>(data); }
        const char *get_buffer() const { return reinterpret_cast<const char *>(data); }

        memory::IAllocator *get_allocator() const
        {
            return reinterpret_cast<memory::IAllocator *>(reinterpret_cast<u64>(allocator) & ~0x1);
        }
        void set_allocator(memory::IAllocator *alc)
        {
            allocator = reinterpret_cast<memory::IAllocator *>(reinterpret_cast<u64>(alc) | 0x1);
        }

        bool is_stack() const { return reinterpret_cast<u64>(allocator) & 0x1; }
    };
    union
    {
        stack_t stack;
        head_t head;
    };

    static_assert(sizeof(stack_t) == sizeof(head_t));

  private:
    u64 select_capacity(u64 capacity)
    {
        u64 new_cap = capacity;
        return new_cap;
    }

    void free()
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

    void copy(const string &rhs)
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

    void move(string &&rhs)
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

    void init(memory::IAllocator *allocator, const char *str, i64 len = -1)
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

    void init_lit(const char *str)
    {
        head.init();
        u64 l = strlen(str);
        head.set_allocator(nullptr);
        head.set_count(l);
        head.set_cap(l + 1);
        // readonly string
        head.set_buffer(const_cast<char *>(str));
    }

    u64 get_count() const
    {
        if (likely(is_sso()))
            return stack.get_count();
        else
            return head.get_count();
    }

  private:
    bool is_sso() const { return stack.is_stack(); }

  public:
    class iterator
    {
        char *beg;
        char *end;

      public:
        char &operator*() { return *beg; }
        char *operator->() { return beg; }
        char *operator&() { return beg; }

        iterator operator++(int)
        {
            auto old = *this;
            beg++;
            return old;
        }

        iterator &operator++()
        {
            beg++;
            return *this;
        }

        bool operator==(const iterator &it) { return it.beg == beg; }

        bool operator!=(const iterator &it) { return it.beg != beg; }

        iterator(char *t)
            : beg(t)
        {
        }
    };
};

} // namespace util
