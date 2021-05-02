#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
#include "hash.hpp"
#include <utility>
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
    u64 operator()(const unsigned long &t) { return murmur_hash2_64(&t, sizeof(unsigned long), 0); }
};

template <> struct member_hash<unsigned int>
{
    u64 operator()(const unsigned int &t)
    {
        u64 k = t;
        return murmur_hash2_64(&k, sizeof(k), 0);
    }
};

template <> struct member_hash<long>
{
    u64 operator()(const long &t) { return murmur_hash2_64(&t, sizeof(unsigned long), 0); }
};

template <> struct member_hash<int>
{
    u64 operator()(const int &t)
    {
        u64 k = t;
        return murmur_hash2_64(&k, sizeof(k), 0);
    }
};

template <typename K, typename V, typename hash_func = member_hash<K>> class hash_map
{
  public:
    struct pair;
    struct iterator;
    struct map_helper;
    struct node_t;
    struct entry;

    hash_map(memory::IAllocator *allocator, u64 capacity, u64 factor)
        : count(0)
        , table(nullptr)
        , load_factor(factor)
        , allocator(allocator)
    {
        cap = select_capcity(capacity);
        if (cap > 0) {
            table = memory::NewArray<entry>(allocator, cap);
        }
    }

    hash_map(memory::IAllocator *allocator, std::initializer_list<pair> il)
           : hash_map(allocator, il.size()) {
        for(auto e : il) 
        {
            insert(std::move(e));
        }
    }

    hash_map(memory::IAllocator *allocator, u64 capacity)
        : hash_map(allocator, capacity, 75)
    {
    }

    hash_map(memory::IAllocator *allocator)
        : hash_map(allocator, 0, 75)
    {
    }

    ~hash_map()
    {
        free();
    }

    hash_map(const hash_map &rhs)
    {
        copy(rhs);
    }

    hash_map(hash_map &&rhs) {
        move(std::move(rhs));
    }

    hash_map &operator=(const hash_map &rhs)
    {
        if (&rhs == this)
            return *this;

        free();
        copy(rhs);
        return *this;
    }

    hash_map &operator=(hash_map &&rhs)
    {
        if (&rhs == this)
            return *this;

        free();
        move(std::move(rhs));
        return *this;
    }

    iterator insert(K && k, V && v)
    {
        return insert(std::move(pair(std::move(k), std::move(v))));
    }

    bool insert_once(K &&k, V && v)
    {
        return insert_once(std::move(pair(std::move(k),std::move(v))));
    } 

    iterator insert(pair && v)
    {
        check_capacity(count + 1);
        u64 hash = hash_key(v.key);
        auto next_node = table[hash].next;
        table[hash].next = memory::New<node_t>(allocator, std::move(v), next_node);
        count++;
        return iterator(&table[hash], table + cap, table[hash].next);
    }

    bool insert_once(pair && v)
    {
        check_capacity(count + 1);
        u64 hash = hash_key(v.key);
        for (auto it = table[hash].next; it != nullptr; it = it->next)
        {
            if (it->key == v.key)
                return false;
        }
        insert(std::move(v));
        return true;
    }

    bool get(const K &key, V *v)
    {
        if (unlikely(count == 0)) {
            return false;
        }
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
        if (unlikely(count == 0)) {
            return false;
        }
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
        if (unlikely(count == 0)) {
            return 0;
        }
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
        if (unlikely(count == 0)) {
            return;
        }
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
        if (unlikely(count == 0)) {
            return;
        }
        u64 hash = hash_key(key);
        u64 del = 0;

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
                del ++;
                memory::Delete<>(allocator, cur_node);
                continue;
            }
            prev = it;
            it = it->next;
        }
        check_capacity(count - del);
        count -= del;
    }

    void remove_once(const K &key)
    {
        if (unlikely(count == 0)) {
            return;
        }
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
                check_capacity(count --);
                return;
            }
            prev = it;
            it = it->next;
        }
    }

    map_helper operator[](const K &key)
    {
        if (likely(count > 0)) {
            u64 hash = hash_key(key);
            for (auto it = table[hash].next; it != nullptr; it = it->next)
            {
                if (it->content.key == key)
                {
                    return map_helper(it);
                }
            }
        }

        K k = key;
        return map_helper(insert(std::move(k), std::move(V())).node);
    }

    u64 size() const { return count; }

    void clear()
    {
        if (unlikely(count == 0)) {
            return;
        }
        for (u64 i = 0; i < cap; i++)
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

    u64 capacity() const {
        return cap;
    }

    iterator begin() const
    {
        auto table = this->table;
        if (table == nullptr) {
            return end();
        }
        auto node = this->table->next;
        if (!node)
        {
            while (!table->next && table != (this->table + cap))
            {
                table++;
            }
            node = table->next;
        }
        return iterator(table, this->table + cap, node);
    }

    iterator end() const { return iterator(table + cap, table + cap, nullptr); }

  private:
    u64 count;
    entry *table;
    // fix point 12345.67
    u64 cap;
    u8 load_factor;
    memory::IAllocator *allocator;

    void recapcity(u64 new_capacity)
    {
        if (unlikely(new_capacity == cap)) {
            return;
        }
        auto new_table = memory::NewArray<entry>(allocator, new_capacity);
        for (u64 i = 0; i < cap; i++)
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
        if (table != nullptr) {
            memory::Delete<>(allocator, table);
        }
        table = new_table;
        cap = new_capacity;
    }

    u64 select_capcity(u64 capacity) {
        return capacity;
    }

    u64 hash_key(const K &key) { 
        return hash_func()(key) % cap; 
    }

    void check_capacity(u64 new_count) {
        if(unlikely(cap == 0 && new_count > 0)) 
        {
            recapcity(2);
        }
        else if (new_count >= cap * load_factor / 100)
        {
            recapcity(cap * 2);
        } 
    }

    void free() {
        if (table != nullptr) {
            clear();
            memory::Delete<>(allocator, table);
            table = nullptr;
        }
    }

    void copy(const hash_map &rhs) {
        allocator = rhs.allocator;
        load_factor = rhs.load_factor;
        cap = select_capcity(rhs.count);
        count = 0;
        table = memory::NewArray<entry>(allocator, cap); 
        auto iter = rhs.begin();
        while(iter != rhs.end()) {
            auto v = *iter;
            insert(std::move(v));
            iter++;
        }
    }

    void move(hash_map && rhs) {
        allocator = rhs.allocator;
        table = rhs.table;
        count = rhs.count;
        load_factor = rhs.load_factor;
        cap = rhs.cap;
        rhs.table = nullptr;
        rhs.count = 0;
        rhs.cap = 0;
        rhs.load_factor = 0;
    }

  public:
    struct pair
    {
        K key;
        V value;

        pair(K &&key, V &&value)
            : key(key)
            , value(value){};

        pair(const K &key, const V &value)
            : key(key)
            , value(value){};
    };
    struct node_t
    {
        pair content;
        node_t *next;
        pair &operator*() { return content; }

        node_t(pair && p, node_t *next)
            : content(std::move(p))
            , next(next){};
    };

    struct entry
    {
        node_t *next;
        entry(): next(nullptr) {
        }
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
        pair *operator->() { return &e->content; }
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
            else {
                node = node->next;
            }

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
            else {
                node = node->next;
            }
            return *this;
        }

        bool operator==(const iterator &it) { return node == it.node; }

        bool operator!=(const iterator &it) { return !operator==(it); }

        pair *operator->() { return &node->content; }

        pair &operator*() { return node->content; }

        pair *operator&() { return &node->content; }
    };
};

// template <typename K> using hash_set = hash_map<K, std::void_t>;

} // namespace util