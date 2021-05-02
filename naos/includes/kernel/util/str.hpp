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

class string_view
{
  public:
    string_view()
        : ptr(nullptr)
        , len(0)
    {
    }
    string_view(char *ptr, u64 len)
        : ptr(ptr)
        , len(len)
    {
    }

    string to_string(memory::IAllocator *allocator);

  private:
    char *ptr;
    u64 len;
};

/// \brief kernel string type, use it everywhere
///
class string
{
  public:
    class iterator;

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

    array<string_view> split(char c, memory::IAllocator *vec_allocator);

    bool to_int(i64 &out) const;
    bool to_uint(u64 &out) const;

    bool to_int_ext(i64 &out, string_view &last_view) const;
    bool to_uint_ext(u64 &out, string_view &last_view) const;

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

  public:
    class iterator
    {
        char *beg;

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

        explicit iterator(char *t)
            : beg(t)
        {
        }
    };
};

} // namespace util
