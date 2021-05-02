#include "kernel/util/singly_linked_list.hpp"
#include "test.hpp"

using namespace util;

void test_slist_create()
{
    singly_linked_list<int> list(&LibAllocatorV);
    assert(list.size() == 0);
}
test(slist, create);

void test_slist_push()
{
    singly_linked_list<int> list(&LibAllocatorV);
    list.push_back(2);
    list.push_front(3);
    list.push_back(1);
    assert(list.size() == 3 && list.at(0) == 3);
    assert(list.at(1) == 2);
    assert(list.at(2) == 1);
}
test(slist, push);

void test_slist_insert()
{
    singly_linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    list.insert(++list.begin(), std::move(-1));
    list.insert(list.end(), std::move(-2));
    assert(list.size() == 5);
    assert(list.at(0) == 1);
    assert(list.at(1) == -1);
    assert(list.at(2) == 2);
    assert(list.at(3) == 3);
    assert(list.at(4) == -2);
}
test(slist, insert);

void test_slist_remove()
{
    singly_linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    list.remove(list.begin());
    assert(list.size() == 2 && list.at(1) == 3);
    list.remove(++list.begin());
    assert(list.size() == 1 && list.at(0) == 2);
}
test(slist, remove);

void test_slist_iterator()
{
    singly_linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    int j = 1;
    for (auto i : list)
    {
        assert(i == j);
        j++;
    }
}
test(slist, iterator);

void test_slist_const_iterator()
{
    const singly_linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    int j = 1;
    for (auto i : list)
    {
        assert(i == j);
        j++;
    }
}
test(slist, const_iterator);

void test_slist_empty()
{
    singly_linked_list<int> list(&LibAllocatorV);
    assert(list.empty());
    list.clear();
}
test(slist, empty);

void test_slist_copy()
{
    singly_linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    singly_linked_list<int> list2(list);
    assert(list2.size() == 3 && list.size() == 3);
    list2.push_back(std::move(4));
    list.push_back(std::move(5));
    assert(list2.size() == 4 && list2.at(3) == 4);
    list2 = list;
    assert(list2.size() == 4 && list2.at(3) == 5);
}
test(slist, copy);

void test_slist_move()
{
    singly_linked_list<int> list(&LibAllocatorV, {1, 2, 3});
    singly_linked_list<int> list2(std::move(list));
    assert(list2.size() == 3 && list.size() == 0);
    list2.push_back(std::move(4));
    assert(list2.size() == 4 && list2.at(3) == 4);

    singly_linked_list<int> list3(&LibAllocatorV, {1, 2, 3});
    list3 = std::move(list2);

    assert(list3.size() == 4 && list3.at(3) == 4 && list2.size() == 0);
}
test(slist, move);