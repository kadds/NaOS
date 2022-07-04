#include "kernel/util/skip_list.hpp"
#include "comm.hpp"
#include "test.hpp"

using namespace util;

namespace util
{
u64 next_rand(u64 v) { return rand() % v; }
} // namespace util

void test_skip_list_create()
{
    skip_list<int> list(&LibAllocatorV);
    assert(list.size() == 0);
}
test(skip_list, create);

void test_skip_list_insert()
{
    skip_list<int> list(&LibAllocatorV);
    list.insert(1);
    list.insert(2);
    assert(list.has(1) && list.has(2) && list.size() == 2);
}
test(skip_list, insert);

void test_skip_list_iterator()
{
    skip_list<int> list(&LibAllocatorV);
    for (int i = 0; i < 100; i++)
    {
        int j = i;
        list.insert(std::move(i));
    }
    int j = 0;
    for (auto i : list)
    {
        assert(i == j);
        j++;
    }
}
test(skip_list, iterator);

void test_skip_list_copy()
{
    skip_list<int> list(&LibAllocatorV, {1, 2, 3});
    skip_list<int> list2 = list;
    assert(list2.size() == 3 && list2.has(1) && list2.has(2) && list2.has(3) && list.size() == 3);
    list2.insert(4);
    list.insert(5);
    assert(list2.size() == 4 && list.size() == 4 && list2.has(4));
    list2 = list;
    assert(list2.size() == 4 && list2.has(5));
}
test(skip_list, copy);

void test_skip_list_move()
{
    skip_list<int> list(&LibAllocatorV, {1, 2, 3});
    skip_list<int> list2 = std::move(list);
    assert(list2.size() == 3 && list2.has(1) && list2.has(2) && list2.has(3) && list.size() == 0);
    list2.insert(4);
    assert(list2.size() == 4 && list2.has(4));
    skip_list<int> list3(&LibAllocatorV, {4, 5});
    list3 = std::move(list2);
    assert(list3.size() == 4 && list3.has(4) && list2.size() == 0);
}
test(skip_list, move);

void test_skip_list_find()
{
    skip_list<int> list(&LibAllocatorV, {1, 2, 4});
    auto iter = list.find(2);
    assert(iter.get()->element == 2);
    iter = list.find(3);
    assert(iter == list.end());
    iter = list.find(4);
    assert(iter.get()->element == 4);
}
test(skip_list, find);

void test_skip_list_binary()
{
    skip_list<int> list(&LibAllocatorV, {1, 2, 4});
    auto iter = list.upper_find(2);
    assert(iter.get()->element == 2);

    iter = list.lower_find(2);
    assert(iter.get()->element == 4);

    iter = list.upper_find(3);
    assert(iter.get()->element == 4);
    iter = list.upper_find(4);
    assert(iter.get()->element == 4);
}
test(skip_list, binary);
