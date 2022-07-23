#include "kernel/util/hash_map.hpp"
#include "test.hpp"
#include <unordered_map>

using namespace util;

void test_hash_map_create()
{
    hash_map<int, int> map(&LibAllocatorV);
    assert(map.size() == 0);
}
test(hash_map, create);

void test_hash_map_insert()
{
    hash_map<int, int> map(&LibAllocatorV);
    map.insert(1, 1);
    map.insert(2, -1);
    map.insert(3, 5);
    map.insert(4, 6);
    assert(map.size() == 4);
    int v;
    assert(map.get(1, v) && v == 1);
    assert(map.get(2, v) && v == -1);
    assert(map.get(3, v) && v == 5);
    assert(map.get(4, v) && v == 6);
}
test(hash_map, insert);

void test_hash_map_remove()
{
    hash_map<int, int> map(&LibAllocatorV, {{1, 2}, {3, 4}, {5, 6}});
    map.remove(1);
    assert(!map.has(1));
    map.remove(5);
    assert(!map.has(5));
    map.remove(6);
    assert(map.has(3));
}
test(hash_map, remove);

void test_hash_map_iterator()
{
    hash_map<int, int> map(&LibAllocatorV, {{1, 2}, {3, 4}, {5, 6}});
    std::unordered_map<int, int> m;
    for (int i = 1; i <= 6; i += 2)
    {
        m.insert(std::make_pair(i, i + 1));
    }
    for (auto i : map)
    {
        assert(m[i.key] == i.value);
    }
}
test(hash_map, iterator);

void test_hash_map_copy()
{
    hash_map<int, int> map(&LibAllocatorV, {{1, 2}, {3, 4}, {5, 6}});
    hash_map<int, int> map2(map);
    assert(map2.size() == 3 && map.size() == 3);
    map2.insert(std::move(4), std::move(0));
    map.insert(std::move(5), std::move(0));
    assert(map2.size() == 4 && map2.has(4) && !map.has(4));
    map2 = map;
    assert(map2.size() == 4 && map2.has(5));
}
test(hash_map, copy);

void test_hash_map_move()
{
    hash_map<int, int> map(&LibAllocatorV, {{1, 2}, {3, 4}, {5, 6}});
    hash_map<int, int> map2(std::move(map));
    assert(map2.size() == 3 && map.size() == 0);
    map2.insert(std::move(4), std::move(0));
    assert(map2.size() == 4 && map2.has(4));

    hash_map<int, int> map3(&LibAllocatorV, {{1, 2}});
    map3 = std::move(map2);
    assert(map3.size() == 4 && map3.has(4) && map2.size() == 0);
}
test(hash_map, move);

void test_hash_map_set()
{
    hash_set<int> map(&LibAllocatorV, {1, 2, 3});
    assert(map.has(1) && map.has(2) && map.has(3) && !map.has(4));
}
test(hash_map, set);