#include "kernel/util/linked_list.hpp"
#include "test.hpp"

using namespace util;

void test_list_create()
{
    linked_list<int> list(&LibAllocatorV);
    assert(list.size() == 0);
}
test(list, create);

void test_list_push()
{
    linked_list<int> list(&LibAllocatorV);
    list.push_back(2);
    list.push_front(3);
    list.push_back(1);
    assert(list.size() == 3 && list.at(0) == 3);
    assert(list.at(1) == 2);
    assert(list.at(2) == 1);
}
test(list, push);

void test_list_insert()
{
    linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    list.insert(++list.begin(), std::move(-1));
    list.insert(list.end(), std::move(-2));
    assert(list.size() == 5);
    assert(list.at(0) == 1);
    assert(list.at(1) == -1);
    assert(list.at(2) == 2);
    assert(list.at(3) == 3);
    assert(list.at(4) == -2);
}
test(list, insert);

void test_list_remove()
{
    linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    list.remove(list.begin());
    assert(list.size() == 2 && list.at(1) == 3);
    list.remove(++list.begin());
    assert(list.size() == 1 && list.at(0) == 2);
}
test(list, remove);

void test_list_iterator()
{
    linked_list<int> list(&LibAllocatorV);
    for (int i = 0; i < 100; i++)
    {
        int j = i;
        list.push_back(std::move(j));
    }
    assert(list.size() == 100);
    int j = 0;
    for (auto i : list)
    {
        assert(i == j);
        j++;
    }
}
test(list, iterator);

void test_list_copy()
{
    linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    linked_list<int> list2(list);
    assert(list2.size() == 3 && list.size() == 3);
    list2.push_back(std::move(4));
    list.push_back(std::move(5));
    assert(list2.size() == 4 && list2.at(3) == 4);
    list2 = list;
    assert(list2.size() == 4 && list2.at(3) == 5);
}
test(list, copy);

void test_list_move()
{
    linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    linked_list<int> list2(std::move(list));
    assert(list2.size() == 3 && list.size() == 0);
    list2.push_back(std::move(4));
    assert(list2.size() == 4 && list2.at(3) == 4);

    linked_list<int> list3(&LibAllocatorV, {1, 2, 3});
    list3 = std::move(list2);

    assert(list3.size() == 4 && list3.at(3) == 4 && list2.size() == 0);
}
test(list, move);