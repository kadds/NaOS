#pragma once
#include "memory.hpp"
namespace util
{
template <typename BaseType, int BitPreElement> class bit_set
{
  private:
    using raw_type = BaseType;
    constexpr static u64 SizePreElement = (BitPreElement + 8 - 1) / 8;
    memory::IAllocator *allocator;
    raw_type *data;
    int element_count;
    static_assert((BitPreElement & (BitPreElement - 1)) == 0, "BitPreElement can only be a power of 2.");
    static_assert(BitPreElement <= sizeof(raw_type) * 8, "BitPreElement is bigger than BaseType.");

  public:
    bit_set(memory::IAllocator *allocator, int element_count)
        : allocator(allocator)
        , element_count(element_count)

    {

        u64 bytes = (element_count * BitPreElement + 7) / 8;
        bytes = (bytes + sizeof(raw_type) - 1) & ~(sizeof(raw_type) - 1);
        data = memory::NewArray<raw_type>(allocator, bytes / sizeof(raw_type));
    }
    bit_set(const bit_set &v) {}
    ~bit_set()
    {
        int bytes = (element_count * BitPreElement + 7) / 8;
        bytes = (bytes + sizeof(raw_type) - 1) & ~(sizeof(raw_type) - 1);
        memory::DeleteArray<raw_type>(allocator, data, bytes / sizeof(raw_type));
    }
    BaseType get(int index)
    {
        kassert(index < element_count, "out of range");

        auto bit = BitPreElement * index; // which bit
        auto i8 = bit / 8;
        auto rest = bit % 8;
        auto v = data[i8 / sizeof(raw_type)];
        v >>= (i8 % sizeof(raw_type) * 8 + rest);
        v &= ((1 << BitPreElement) - 1);
        return v;
    }
    void set(int index, BaseType value)
    {
        kassert(index < element_count, "out of range");

        value &= (1 << BitPreElement) - 1;
        auto bit = BitPreElement * index; // which bit
        auto i8 = bit / 8;
        auto rest = bit % 8;
        auto &v = data[i8 / sizeof(raw_type)];
        int offset = i8 % sizeof(raw_type) * 8 + rest;

        u64 x = ((1lu << offset) - 1);
        if (offset + BitPreElement < 64)
        {
            x |= ~((1lu << (offset + BitPreElement)) - 1);
        }

        v = (v & x) | (value << offset);
    }
    void clean(int index) { set(index, 0); }
    void clean_all()
    {
        int bytes = element_count * SizePreElement / 8;
        util::memzero(data, bytes);
    }

    int count() const { return element_count; }
};
template <typename BaseType, int BitPreElement, int ElementCount> class bit_set_fixed
{
    using raw_type = BaseType;
    raw_type data[((BitPreElement * ElementCount + 7) / 8 + sizeof(raw_type) - 1) / sizeof(raw_type)];

  public:
    bit_set_fixed() {}

    ~bit_set_fixed() {}
    BaseType get(int index)
    {
        kassert(index < ElementCount, "out of range");

        auto bit = BitPreElement * index; // which bit
        auto i8 = bit / 8;
        auto rest = bit % 8;
        auto v = data[i8 / sizeof(raw_type)];
        v >>= (i8 % sizeof(raw_type) * 8 + rest);
        v &= ((1 << BitPreElement) - 1);
        return v;
    }
    void set(int index, BaseType value)
    {
        kassert(index < ElementCount, "out of range");

        value &= (1 << BitPreElement) - 1;
        auto bit = BitPreElement * index; // which bit
        auto i8 = bit / 8;
        auto rest = bit % 8;
        auto &v = data[i8 / sizeof(raw_type)];
        int offset = i8 % sizeof(raw_type) * 8 + rest;

        u64 x = ((1lu << offset) - 1);
        if (offset + BitPreElement < 64)
        {
            x |= ~((1lu << (offset + BitPreElement)) - 1);
        }

        v = (v & x) | (value << offset);
    }
    void clean(int index) { set(index, 0); }
    void clean_all()
    {
        int bytes = ElementCount * BitPreElement / 8;
        util::memzero(data, bytes);
    }
    int count() const { return ElementCount; }
};
} // namespace util
