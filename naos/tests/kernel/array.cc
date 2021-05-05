#include "kernel/util/array.hpp"
#include "test.hpp"

using namespace util;

void test_array_create()
{
    array<int> vec(&LibAllocatorV);
    assert(vec.size() == 0);
}
test(array, create);

void test_array_push()
{
    array<int> vec(&LibAllocatorV);
    vec.push_back(2);
    vec.push_front(3);
    vec.push_back(1); // 3 2 1
    assert(vec.size() == 3 && vec[0] == 3);
    assert(vec[1] == 2);
    assert(vec[2] == 1);
}
test(array, push);

void test_array_remove()
{
    array<int> vec(&LibAllocatorV, {1, 2, 3});
    vec.remove_at(2); // 1 2
    assert(vec.size() == 2 && vec[1] == 2);
    vec.remove_at(0); // 2
    assert(vec.size() == 1 && vec[0] == 2);
    vec.remove_at(0);
    assert(vec.size() == 0);
}
test(array, remove);

void test_array_insert()
{
    array<int> vec(&LibAllocatorV, {1, 2, 3});
    vec.insert(++vec.begin(), -1);
    vec.insert(vec.end(), -2);
    assert(vec.size() == 5);
    assert(vec[0] == 1);
    assert(vec[1] == -1);
    assert(vec[2] == 2);
    assert(vec[3] == 3);
    assert(vec[4] == -2);
}
test(array, insert);

void test_array_remove_range()
{
    array<int> vec(&LibAllocatorV, {1, 2, 3, 4, 5});
    vec.remove_at(1, 3);
    assert(vec.size() == 3 && vec[1] == 4);
}
test(array, remove_range);

void test_array_resize()
{
    array<int> vec(&LibAllocatorV, {1, 2, 3});
    vec.resize(4, -1);
    assert(vec.size() == 4 && vec[3] == -1);
    vec.resize(2, 0);
    assert(vec.size() == 2 && vec[1] == 2);
}
test(array, resize);

void test_array_iterator()
{
    array<int> vec(&LibAllocatorV, {1, 2, 3});
    int j = 1;
    for (auto &i : vec)
    {
        assert(i == j);
        i = j + 1;
        j++;
    }
}
test(array, iterator);

void test_array_const_iterator()
{
    const array<int> vec(&LibAllocatorV, {1, 2, 3});
    int j = 1;
    for (auto &i : vec)
    {
        assert(i == j);
        j++;
    }
}

test(array, const_iterator);

void test_array_empty()
{
    array<int> vec(&LibAllocatorV);
    assert(vec.empty());
    vec.clear();
}
test(array, empty);

void test_array_copy()
{
    array<int> vec(&LibAllocatorV, {1, 2, 3});
    array<int> vec2 = vec;
    assert(vec2.size() == 3 && vec2[0] == 1 && vec.size() == 3);
    vec2.push_back(4);
    vec.push_back(5);
    assert(vec2.size() == 4 && vec2[3] == 4);
    vec2 = vec;
    assert(vec2.size() == 4 && vec2[3] == 5);
}
test(array, copy);

void test_array_expand()
{
    array<int> vec(&LibAllocatorV);
    vec.ensure(10000);
    for (int i = 0; i < 10000; i++)
    {
        vec.push_back(i);
    }
    vec.truncate(400);
    assert(vec.size() == 400);
    vec.expand(402, 1);
    assert(vec.size() == 402 && vec[401] == 1);
    vec.shrink(vec.size());
    assert(vec.capacity() == 402);
}

test(array, expand);

void test_array_move()
{
    array<int> vec(&LibAllocatorV, {1, 2, 3});
    auto d0 = vec.data();
    array<int> vec2 = std::move(vec);
    auto d1 = vec2.data();
    assert(vec2.size() == 3 && vec.size() == 0 && d0 == d1);
    vec2.push_back(4);

    array<int> vec3(&LibAllocatorV, {4, 5});
    vec3 = std::move(vec2);

    assert(vec3.size() == 4 && vec3[3] == 4 && vec2.size() == 0);
}
test(array, move);

void test_array_obj()
{
    array<Int> vec(&LibAllocatorV, {1, 2, 3});
    vec.push_back(4, 6);
    vec.insert_at(0, 0, 0);
    vec.ensure(10);
    vec.remove_at(1);
    assert(vec[3] == Int(4, 6));
}
test(array, obj);
