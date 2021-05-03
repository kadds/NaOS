#pragma once
#include <concepts>

namespace util
{
template <typename C> concept iterator_container = requires(C c) { c.iter(); };

template <typename T, typename E> concept for_each_iterator = requires(T t)
{
    t.begin();
    t.end();
};

template <typename T> concept forward_iterator = requires(T t) { t.next(); };

template <typename T> concept bidirectional_iterator = requires(T t)
{
    t.next();
    t.prev();
};
template <typename T> concept random_iterator = requires(T t) { t.at(1); };

template <typename T, typename E> requires forward_iterator<T> T find(T beg, T end, const E &val)
{
    while (beg != end)
    {
        if (*beg == val)
        {
            return beg;
        }
        beg++;
    }
    return beg;
}

template <typename CE> class base_bidirectional_iterator
{
  private:
    CE current;

  public:
    explicit base_bidirectional_iterator(CE e)
        : current(e)
    {
    }

  public:
    auto &operator*() { return *current; }
    auto operator&() { return current; }
    auto operator->() { return current; }

    auto &next() { return *current++; }
    auto &prev() { return *current--; }

    base_bidirectional_iterator operator++(int)
    {
        current++;
        return base_bidirectional_iterator(current - 1);
    }
    base_bidirectional_iterator &operator++()
    {
        current++;
        return *this;
    }
    base_bidirectional_iterator operator--(int)
    {
        current--;
        return base_bidirectional_iterator(current + 1);
    }
    base_bidirectional_iterator &operator--()
    {
        current--;
        return *this;
    }
    bool operator==(const base_bidirectional_iterator &it) const { return current == it.current; }

    bool operator!=(const base_bidirectional_iterator &it) const { return !operator==(it); }
};
} // namespace util