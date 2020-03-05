#pragma once
#include "../mm/new.hpp"
#include "../trace.hpp"
#include "../util/memory.hpp"

namespace util
{
namespace bit_set_opeartion
{
using BaseType = u64;

bool get(BaseType *data, u64 index);

void set(BaseType *data, u64 index);
/// set [0, element_count) to 1
void set_all(BaseType *data, u64 element_count);
/// set [start, start+element_count) to 1
void set_all(BaseType *data, u64 start, u64 element_count);

void clean(BaseType *data, u64 index);
/// set [0, element_count) to 0
void clean_all(BaseType *data, u64 element_count);
/// set [start, start+element_count) to 0
void clean_all(BaseType *data, u64 start, u64 element_count);
/// scan [offset, offset+max_len)
u64 scan_zero(BaseType *data, u64 offset, u64 max_len);

u64 scan_set(BaseType *data, u64 offset, u64 max_len);

}; // namespace bit_set_opeartion
class bit_set
{
  private:
    memory::IAllocator *allocator;
    // data pointer
    u64 *data;
    u64 element_count;

  public:
    bit_set(memory::IAllocator *allocator, u64 element_count)
        : allocator(allocator)
        , element_count(element_count)
    {

        u64 bytes = (element_count + 7) / 8;
        data = (u64 *)allocator->allocate(bytes, 1);
    }

    bit_set(const bit_set &v) = delete;
    bit_set &operator=(const bit_set &v) = delete;

    ~bit_set() { allocator->deallocate(data); }

    void clean_all() { bit_set_opeartion::clean_all(data, element_count); }

    void set_all() { bit_set_opeartion::set_all(data, element_count); }

    bool get(u64 index)
    {
        kassert(index < element_count, "Index is out of range. index:", index, " maximum:", element_count);
        return bit_set_opeartion::get(data, index);
    }

    void set(u64 index)
    {
        kassert(index < element_count, "Index is out of range. index:", index, " maximum:", element_count);
        bit_set_opeartion::set(data, index);
    }

    void set_all(u64 index, u64 count)
    {
        kassert(index < element_count, "Index is out of range. index:", index, " maximum:", element_count);
        kassert(index + count < element_count, "Index + Count is out of range:", index, "+", count,
                " maximum:", element_count);
        bit_set_opeartion::set_all(data, index, count);
    }

    void clean(u64 index)
    {
        kassert(index < element_count, "Index is out of range. index:", index, " maximum:", element_count);
        bit_set_opeartion::clean(data, index);
    }

    void clean_all(u64 index, u64 count)
    {
        kassert(index < element_count, "Index is out of range. index:", index, " maximum:", element_count);
        kassert(index + count < element_count, "Index + Count is out of range:", index, "+", count,
                " maximum:", element_count);
        bit_set_opeartion::clean_all(data, index, count);
    }

    u64 scan_zero(u64 offset, u64 max_len)
    {
        kassert(offset < element_count, "Offset is out of range. offset:", offset, " maximum:", element_count);
        kassert(offset + max_len < element_count, "Offset + Count is out of range:", offset, "+", max_len,
                " maximum:", element_count);
        return bit_set_opeartion::scan_zero(data, offset, max_len);
    }

    u64 scan_zero() { return bit_set_opeartion::scan_zero(data, 0, element_count); }

    u64 scan_set(u64 offset, u64 max_len)
    {
        kassert(offset < element_count, "Offset is out of range. offset:", offset, " maximum:", element_count);
        kassert(offset + max_len < element_count, "Offset + Count is out of range:", offset, "+", max_len,
                " maximum:", element_count);
        return bit_set_opeartion::scan_set(data, offset, max_len);
    }

    u64 scan_set() { return bit_set_opeartion::scan_set(data, 0, element_count); }

    u64 count() const { return element_count; }
};

template <int ElementCount> class bit_set_inplace
{
    u64 data[(ElementCount + 63) / 64];

  public:
    bit_set_inplace() = default;
    bit_set_inplace(u64 map) { set_to(map); }

    ~bit_set_inplace() = default;

    u64 *get_ptr() { return data; }

    void set_to(u64 map)
    {
        if (ElementCount > sizeof(u64) * 8)
        {
            /// FIXME: set mask
        }
        data[0] = map;
    }

    void clean_all() { bit_set_opeartion::clean_all(data, ElementCount); }

    void set_all() { bit_set_opeartion::set_all(data, ElementCount); }

    bool get(u64 index)
    {
        kassert(index < ElementCount, "Index is out of range. index:", index, " maximum:", ElementCount);
        return bit_set_opeartion::get(data, index);
    }

    void set(u64 index)
    {
        kassert(index < ElementCount, "Index is out of range. index:", index, " maximum:", ElementCount);
        bit_set_opeartion::set(data, index);
    }

    void set_all(u64 index, u64 count)
    {
        kassert(index < ElementCount, "Index is out of range. index:", index, " maximum:", ElementCount);
        kassert(index + count < ElementCount, "Index + Count is out of range:", index, "+", count,
                " maximum:", ElementCount);
        bit_set_opeartion::set_all(data, index, count);
    }

    void clean(u64 index)
    {
        kassert(index < ElementCount, "Index is out of range. index:", index, " maximum:", ElementCount);
        bit_set_opeartion::clean(data, index);
    }

    void clean_all(u64 index, u64 count)
    {
        kassert(index < ElementCount, "Index is out of range. index:", index, " maximum:", ElementCount);
        kassert(index + count < ElementCount, "Index + Count is out of range:", index, "+", count,
                " maximum:", ElementCount);
        bit_set_opeartion::clean_all(data, index, count);
    }

    u64 scan_zero(u64 offset, u64 max_len)
    {
        kassert(offset < ElementCount, "Offset is out of range. offset:", offset, " maximum:", ElementCount);
        kassert(offset + max_len < ElementCount, "Offset + Count is out of range:", offset, "+", max_len,
                " maximum:", ElementCount);
        return bit_set_opeartion::scan_zero(data, offset, max_len);
    }

    u64 scan_zero() { return bit_set_opeartion::scan_zero(data, 0, ElementCount); }

    u64 scan_set(u64 offset, u64 max_len)
    {
        kassert(offset < ElementCount, "Offset is out of range. offset:", offset, " maximum:", ElementCount);
        kassert(offset + max_len < ElementCount, "Offset + Count is out of range:", offset, "+", max_len,
                " maximum:", ElementCount);
        return bit_set_opeartion::scan_set(data, offset, max_len);
    }

    u64 scan_set() { return bit_set_opeartion::scan_set(data, 0, ElementCount); }

    u64 count() const { return ElementCount; }
};
} // namespace util
