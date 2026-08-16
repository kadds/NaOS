#include "freelibcxx/skip_list.hpp"

#include <cassert>
#include <cstddef>
#include <cstdlib>

namespace
{
class test_allocator final : public freelibcxx::Allocator
{
  public:
    void *allocate(size_t size, size_t) noexcept override { return std::malloc(size); }
    void deallocate(void *pointer) noexcept override { std::free(pointer); }
};

struct cfs_vtime_key
{
    unsigned long vtime;
    unsigned long tid;

    cfs_vtime_key(unsigned long vtime, unsigned long tid)
        : vtime(vtime)
        , tid(tid)
    {
    }

    bool operator<(const cfs_vtime_key &other) const
    {
        if (vtime != other.vtime)
            return vtime < other.vtime;
        return tid < other.tid;
    }
    bool operator==(const cfs_vtime_key &other) const { return tid == other.tid; }
};
} // namespace

int run_cfs_vtime_order_tests()
{
    test_allocator allocator;
    freelibcxx::skip_list<cfs_vtime_key> runnable(&allocator, 0);
    runnable.insert(0, 1);
    runnable.insert(0, 2);
    runnable.insert(0, 3);

    assert(runnable.find({0, 1}) != runnable.end());
    assert(runnable.find({0, 2}) != runnable.end());
    assert(runnable.find({0, 3}) != runnable.end());
    return 0;
}
