#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
#include "memory.hpp"
#include <type_traits>
namespace util
{

template <typename T> struct member_hash
{
    u64 operator()(const T &t) { return t.hash(); }
};

template <> struct member_hash<unsigned long>
{
    u64 operator()(const unsigned long &t) { return t; }
};

template <> struct member_hash<unsigned int>
{
    u64 operator()(const unsigned int &t) { return (u64)t; }
};

template <> struct member_hash<long>
{
    u64 operator()(const long &t) { return (u64)t; }
};

template <> struct member_hash<int>
{
    u64 operator()(const int &t) { return (u64)t; }
};

template <typename K, typename V, typename hash_func = member_hash<K>> class hash_map
{
  public:
    struct pair
    {
        K key;
        V value;

        pair(const K &key, const V &value)
            : key(key)
            , value(value){};
    };
    struct node_t
    {
        pair content;
        node_t *next;
        pair &operator*() { return content; }

        node_t(const K &key, const V &value, node_t *next)
            : content(key, value)
            , next(next){};
    };

    struct entry
    {
        node_t *next;
    };

    struct map_helper
    {
        node_t *e;
        map_helper(node_t *e)
            : e(e)
        {
        }
        map_helper &operator=(const V &v)
        {
            e->content.value = v;
            return *this;
        }
        map_helper &operator=(const map_helper &v)
        {
            e->content.value = v.e->content.value;
            return *this;
        }
    };

    struct iterator
    {
        entry *table, *end_table;
        node_t *node;
        iterator(entry *table, entry *end_table, node_t *node)
            : table(table)
            , end_table(end_table)
            , node(node)
        {
        }

        iterator operator++(int)
        {
            auto old = *this;
            auto node = this->node;
            auto table = this->table;

            if (!node->next)
            {
                node = node->next;
                while (!node)
                {
                    table++;
                    if (table >= end_table)
                        return old;
                    else
                        node = table->next;
                }
            }
            else
                node = node->next;

            return old;
        }

        iterator &operator++()
        {
            if (!node->next)
            {
                node = node->next;
                while (!node)
                {
                    table++;
                    if (table >= end_table)
                        break;
                    else
                        node = table->next;
                }
            }
            else
                node = node->next;
            return *this;
        }

        bool operator==(const iterator &it) { return node == it.node; }

        bool operator!=(const iterator &it) { return !operator==(it); }

        pair *operator->() { return &node->content; }

        pair &operator*() { return node->content; }

        pair *operator&() { return &node->content; }
    };

  private:
    u64 count;
    entry *table;
    // fix point 12345.67
    u64 load_factor;
    u64 threshold;
    u64 capacity;
    memory::IAllocator *allocator;

    void recapcity(entry *new_table, u64 new_capacity)
    {
        for (u64 i = 0; i < capacity; i++)
        {
            for (auto it = table[i].next; it != nullptr;)
            {
                u64 hash = hash_func()(it->content.key) % new_capacity;
                auto next_it = it->next;
                auto next_node = new_table[hash].next;
                new_table[hash].next = it;
                it->next = next_node;
                it = next_it;
            }
        }
        memory::KernelMemoryAllocatorV->deallocate(table);
        table = new_table;
        capacity = new_capacity;
        threshold = load_factor * new_capacity / 100;
    }

  public:
    hash_map(memory::IAllocator *allocator, u64 capacity, u64 factor)
        : count(0)
        , load_factor(factor)
        , capacity(capacity)
        , allocator(allocator)

    {
        threshold = load_factor * capacity / 100;
        table = (entry *)memory::KernelMemoryAllocatorV->allocate(sizeof(entry) * capacity, alignof(entry));
        memzero(table, sizeof(entry) * capacity);
    }

    hash_map(memory::IAllocator *allocator, u64 capacity)
        : hash_map(allocator, capacity, 75)
    {
    }

    hash_map(memory::IAllocator *allocator)
        : hash_map(allocator, 11, 75)
    {
    }

    hash_map(const hash_map &map)
        : count(0)
        , allocator(map.allocator)
        , capacity(map.capacity)
        , load_factor(map.load_factor)
    {
    }

    hash_map &operator=(const hash_map &map)
    {
        if (&map == this)
            return *this;
        clear();

        return *this;
    }

    ~hash_map()
    {
        clear();
        memory::KernelMemoryAllocatorV->deallocate(table);
    }

    u64 hash_key(const K &key) { return hash_func()(key) % capacity; }

    iterator insert(const K &key, const V &value)
    {
        if (count >= threshold)
        {
            expand(capacity * 1.5);
        }
        u64 hash = hash_key(key);
        auto next_node = table[hash].next;
        table[hash].next = memory::New<node_t>(allocator, key, value, next_node);
        count++;
        return iterator(&table[hash], table + capacity, table[hash].next);
    }

    bool insert_once(const K &key, const V &value)
    {
        u64 hash = hash_key(key);
        for (auto it = table[hash].next; it != nullptr; it = it->next)
        {
            if (it->key == key)
                return false;
        }
        insert(key, value);
        return true;
    }

    bool get(const K &key, V *v)
    {
        u64 hash = hash_key(key);
        for (auto it = table[hash].next; it != nullptr; it = it->next)
        {
            if (it->content.key == key)
            {
                *v = it->content.value;
                return true;
            }
        }
        return false;
    }

    bool has(const K &key)
    {
        u64 hash = hash_key(key);
        for (auto it = table[hash].next; it != nullptr; it = it->next)
        {
            if (it->content.key == key)
                return true;
        }
        return false;
    }

    int key_count(const K &key)
    {
        u64 hash = hash_key(key);
        int i = 0;
        for (auto it = table[hash].next; it != nullptr; it = it->next)
        {
            if (it->content.key == key)
                i++;
        }
        return i;
    }

    void remove(const K &key)
    {
        u64 hash = hash_key(key);

        node_t *prev = nullptr;
        for (auto it = table[hash].next; it != nullptr;)
        {
            if (it->content.key == key)
            {
                auto cur_node = it;
                it = it->next;

                if (likely(prev))
                    prev->next = it;
                else
                    table[hash].next = it;
                memory::Delete<>(allocator, cur_node);
                continue;
            }
            prev = it;
            it = it->next;
        }
    }

    void remove_value(const K &key, const V &v)
    {
        u64 hash = hash_key(key);

        node_t *prev = nullptr;
        for (auto it = table[hash].next; it != nullptr;)
        {
            if (it->content.key == key && it->content.value == v)
            {
                auto cur_node = it;
                it = it->next;

                if (likely(prev))
                    prev->next = it;
                else
                    table[hash].next = it;
                memory::Delete<>(allocator, cur_node);
                continue;
            }
            prev = it;
            it = it->next;
        }
    }

    void remove_once(const K &key)
    {
        u64 hash = hash_key(key);
        node_t *prev = nullptr;
        for (auto it = table[hash].next; it != nullptr;)
        {
            if (it->content.key == key)
            {
                auto cur_node = it;
                it = it->next;

                if (likely(prev))
                    prev->next = it;
                else
                    table[hash].next = it;
                memory::Delete<>(allocator, cur_node);
                return;
            }
            prev = it;
            it = it->next;
        }
    }

    map_helper operator[](const K &key)
    {
        u64 hash = hash_key(key);
        for (auto it = table[hash].next; it != nullptr;)
        {
            if (it->content.key == key)
            {
                return map_helper(it);
            }
        }

        return map_helper(insert(key, V()).node);
    }

    u64 size() const { return count; }

    void clear()
    {
        for (u64 i = 0; i < capacity; i++)
        {
            for (auto it = table[i].next; it != nullptr;)
            {
                auto n = it;
                it = it->next;
                memory::Delete<>(allocator, n);
            }
            table[i].next = nullptr;
        }
        count = 0;
    }

    void shrink(u64 new_capacity)
    {
        if (new_capacity > capacity)
        {
            return;
        }

        if (new_capacity > threshold)
        {
            new_capacity = threshold;
        }
        if (new_capacity < 2)
        {
            new_capacity = 2;
        }

        auto new_table =
            (entry *)memory::KernelMemoryAllocatorV->allocate(sizeof(entry) * new_capacity, alignof(entry));
        util::memzero(table, sizeof(entry) * new_capacity);

        recapcity(new_table, new_capacity);
    }

    void expand(u64 new_capacity)
    {
        if (new_capacity < capacity)
        {
            return;
        }

        auto new_table =
            (entry *)memory::KernelMemoryAllocatorV->allocate(sizeof(entry) * new_capacity, alignof(entry));
        util::memzero(table, sizeof(entry) * new_capacity);

        recapcity(new_table, new_capacity);
    }

    iterator begin()
    {
        auto table = this->table;
        auto node = this->table->next;
        if (!node)
        {
            while (!table->next)
            {
                table++;
            }
            node = table->next;
        }
        return iterator(table, this->table + capacity, node);
    }

    iterator end() { return iterator(table + capacity, table + capacity, nullptr); }
};

// template <typename K> using hash_set = hash_map<K, std::void_t>;

} // namespace util