#pragma once
#include "common.hpp"
#include "singly_linked_list.hpp"
#include <type_traits>
namespace util
{
template <typename T> u64 try_hash(const T &t) { return 0; }

template <typename K, typename V> class hash_map
{
  public:
    struct entry
    {
        K key;
        V value;
        entry(const K &key, const V &value)
            : key(key)
            , value(value){};
    };

    using list_entry_t = singly_linked_list<entry>;

    struct map_helper
    {
        entry *e;
        map_helper(entry *e)
            : e(e)
        {
        }
        map_helper &operator=(const V &v)
        {
            e->value = v;
            return *this;
        }
    };

  private:
    u64 count;
    list_entry_t *table;
    // fix point 12345.67
    const u64 load_factor;
    u64 threshold;
    u64 capacity;
    memory::IAllocator *allocator;

  public:
    hash_map(memory::IAllocator *allocator, u64 capacity, u64 factor)
        : load_factor(factor)
        , capacity(capacity)
        , allocator(allocator)
    {
        threshold = load_factor * capacity / 100;
        table = memory::NewArray<list_entry_t>(allocator, capacity, allocator);
    }

    hash_map(memory::IAllocator *allocator, u64 capacity)
        : hash_map(allocator, capacity, 75)
    {
    }

    hash_map(memory::IAllocator *allocator)
        : hash_map(allocator, 11, 75)
    {
    }

    void insert(const K &key, const V &value)
    {
        u64 hash = try_hash(key) % capacity;
        table[hash].push_front(entry(key, value));
    }

    bool insert_once(const K &key, const V &value)
    {
        u64 hash = try_hash(key) % capacity;
        for (auto it : table[hash])
        {
            if (it.key == key)
            {
                return false;
            }
        }
        table[hash].push_front(entry(key, value));
        return true;
    }

    bool get(const K &key, V *v)
    {
        u64 hash = try_hash(key) % capacity;
        for (auto it : table[hash])
        {
            if (it.key == key)
            {
                *v = it.value;
                return true;
            }
        }
        return false;
    }

    bool has(const K &key)
    {
        u64 hash = try_hash(key) % capacity;
        for (auto it : table[hash])
        {
            if (it.key == key)
                return true;
        }
        return false;
    }

    int key_count(const K &key)
    {
        u64 hash = try_hash(key) % capacity;
        int i = 0;
        for (auto it : table[hash])
        {
            if (it.key == key)
            {
                i++;
            }
        }
        return i;
    }

    void remove(const K &key)
    {
        u64 hash = try_hash(key) % capacity;
        for (auto it = table[hash].begin(); it != table[hash].end();)
        {
            if (it->key == key)
            {
                it = table[hash].remove(it);
            }
            else
                ++it;
        }
    }

    void remove_once(K key)
    {
        u64 hash = try_hash(key) % capacity;
        for (auto it = table[hash].begin(); it != table[hash].end();)
        {
            if (it->key == key)
            {
                table[hash].remove(it);
                return;
            }
            else
                ++it;
        }
    }

    map_helper operator[](const K &key)
    {
        u64 hash = try_hash(key) % capacity;
        for (auto &it : table[hash])
        {
            if (it.key == key)
            {
                return map_helper(&it);
            }
        }
        return map_helper(&table[hash].push_front(entry(key, V())));
    }

    u64 size() const { return count; }
};

// template <typename K> using hash_set = hash_map<K, std::void_t>;

} // namespace util