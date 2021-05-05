#pragma once
#include "array.hpp"
#include "assert.hpp"
#include "common.hpp"
#include "hash.hpp"
#include "iterator.hpp"
#include "kernel/mm/allocator.hpp"
#include "formatter.hpp"
#include "memory.hpp"
#include <utility>

namespace util
{

/// \brief compair two string
/// \return 0 if equal
int strcmp(const char *str1, const char *str2);

/// \brief copy src to dst (include \\0) same as 'strcopy(char *, const char *)'
i64 strcopy(char *dst, const char *src, i64 max_len);

/// \brief copy src to dst (include \\0)
/// \param dst copy to
/// \param src copy from
/// \return copy char count (not include \\0)
i64 strcopy(char *dst, const char *src);

/// \brief get string length (not include \\0)
i64 strlen(const char *str);

/// \brief find substring in str
/// \return -1 if not found
i64 strfind(const char *str, const char *pat);

class string;

template <typename CE> class base_string_view
{
  private:
    struct value_fn
    {
        CE operator()(CE val) { return val; }
    };
    struct prev_fn
    {
        CE operator()(CE val) { return val - 1; }
    };
    struct next_fn
    {
        CE operator()(CE val) { return val + 1; }
    };

  public:
    using iterator = base_bidirectional_iterator<CE, value_fn, prev_fn, next_fn>;

    base_string_view()
        : ptr(nullptr)
        , len(0)
    {
    }
    base_string_view(CE ptr, u64 len)
        : ptr(ptr)
        , len(len)
    {
    }

    CE data() {
        return ptr;
    }

    string to_string(memory::IAllocator *allocator);

    iterator begin() { return iterator(ptr); }

    iterator end() { return iterator(ptr + len); }

    bool to_int(i64 &out) const
    {
        const char *beg = ptr;
        const char *end = ptr + len;
        return formatter::str2int(beg, end, out) != beg;
    }
    bool to_uint(u64 &out) const {
        const char *beg = ptr;
        const char *end = ptr + len;
        return formatter::str2uint(beg, end, out) != beg;
    }

    bool to_int_ext(i64 &out, base_string_view &last) const {
        CE beg = ptr;
        CE end = ptr + len;
        beg = formatter::str2int(beg, end, out);
        last = base_string_view(beg, len - (beg - ptr));
        return beg != ptr;
    }

    bool to_uint_ext(u64 &out, base_string_view &last) const {
        CE beg = ptr;
        CE end = ptr + len;
        beg = const_cast<CE>(formatter::str2uint(beg, end, out));
        last = base_string_view(beg, len - (beg - ptr));
        return beg !=  ptr;
    }

    void split2(base_string_view &v0, base_string_view &v1, iterator iter)
    {
        v0 = base_string_view(ptr, iter.get() - ptr);
        v1 = base_string_view(iter.get() + 1, len - (iter.get() - ptr));
    }

    array<base_string_view<CE>> split(char c, memory::IAllocator *vec_allocator)
    {
        array<base_string_view<CE>> vec(vec_allocator);
        CE p = ptr;
        CE prev = p;
        for (u64 i = 0; i < len; i++)
        {
            if (*p == c)
            {
                if (prev < p)
                {
                    vec.push_back(base_string_view<CE>(prev, p - prev));
                }
                prev = p + 1;
            }
            p++;
        }
        if (prev < p)
        {
            vec.push_back(base_string_view<CE>(prev, p - prev));
        }
        return vec;
    }

  private:
    CE ptr;
    u64 len;
};

using string_view = base_string_view<char *>;
using const_string_view = base_string_view<const char *>;

/// \brief kernel string type, use it everywhere
///
class string
{
  private:
    template <typename N> struct value_fn
    {
        N operator()(N val) { return val; }
    };
    template <typename N> struct prev_fn
    {
        N operator()(N val) { return val - 1; }
    };
    template <typename N> struct next_fn
    {
        N operator()(N val) { return val + 1; }
    };

    using CE = const char *;
    using NE = char *;

  public:
    using const_iterator = base_bidirectional_iterator<CE, value_fn<CE>, prev_fn<CE>, next_fn<CE>>;
    using iterator = base_bidirectional_iterator<NE, value_fn<NE>, prev_fn<NE>, next_fn<NE>>;

    string(const string &rhs) { copy(rhs); }

    string(string &&rhs) { move(std::move(rhs)); }

    ///\brief init empty string ""
    string(memory::IAllocator *allocator);

    ///\brief init from char array
    /// no_shared
    string(memory::IAllocator *allocator, const char *str, i64 len = -1) { init(allocator, str, len); }

    ///\brief init from char array lit
    /// readonly & shared
    string(const char *str) { init_lit(str); }

    ~string() { free(); }

    string &operator=(const string &rhs);

    string &operator=(string &&rhs);

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

    iterator begin() { return iterator(data()); }

    iterator end() { return iterator(data() + size()); }

    const_iterator begin() const { return const_iterator(data()); }

    const_iterator end() const { return const_iterator(data() + size()); }

    string_view view() { return string_view(data(), size()); }

    const_string_view view() const { return const_string_view(data(), size()); }

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

    void append(const string &rhs);

    void append_buffer(const char *buf, u64 length);

    void push_back(char ch);

    char pop_back();

    void remove_at(u64 index, u64 end_index);

    void remove(iterator beg, iterator end)
    {
        u64 index = beg.get() - data();
        u64 index_end = end.get() - data();
        remove_at(index, index_end);
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

    bool operator==(const string &rhs) const;

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
        u64 get_count() const { return get_cap() - static_cast<u64>(data[23]); }
        void set_count(u64 c) { data[23] = static_cast<byte>(get_cap() - c); }
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
    u64 select_capacity(u64 capacity);

    void free();

    void copy(const string &rhs);

    void move(string &&rhs);

    void init(memory::IAllocator *allocator, const char *str, i64 len = -1);

    void init_lit(const char *str);

    u64 get_count() const
    {
        if (likely(is_sso()))
            return stack.get_count();
        else
            return head.get_count();
    }

  private:
    bool is_sso() const { return stack.is_stack(); }
};

template <typename CE> string base_string_view<CE>::to_string(memory::IAllocator *allocator)
{
    return string(allocator, ptr, len);
}

} // namespace util
