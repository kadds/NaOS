#pragma once
#include <concepts>

namespace util
{
template <typename T, typename E> concept for_each_container = requires(T t)
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

template <typename T> concept must_forward_iterator = forward_iterator<T> && !bidirectional_iterator<T>;

template <typename T> concept random_iterator = requires(T t) { t.at(1); }
&&bidirectional_iterator<T>;

template <typename T, typename C> concept check_fn = requires(T t, C c) { t(c); };

template <typename C, typename Val, typename Prev, typename Next>
requires check_fn<Val, C> &&check_fn<Prev, C> &&check_fn<Next, C> class base_bidirectional_iterator
{
  private:
    C current;

  public:
    explicit base_bidirectional_iterator(C c)
        : current(c)
    {
    }

  public:
    C get() { return current; }

    auto &operator*() { return *Val()(current); }
    auto operator&() { return Val()(current); }
    auto operator->() { return Val()(current); }

    base_bidirectional_iterator operator++(int)
    {
        auto old = *this;
        current = Next()(current);
        return old;
    }
    base_bidirectional_iterator &operator++()
    {
        current = Next()(current);
        return *this;
    }
    base_bidirectional_iterator operator--(int)
    {
        auto old = *this;
        current = Prev()(current);
        return old;
    }
    base_bidirectional_iterator &operator--()
    {
        current = Prev()(current);
        return *this;
    }

    base_bidirectional_iterator next()
    {
        auto s = *this;
        s.current = Next()(current);
        return s;
    }
    base_bidirectional_iterator prev()
    {
        auto s = *this;
        s.current = Prev()(current);
        return s;
    }

    bool operator==(const base_bidirectional_iterator &it) const { return current == it.current; }

    bool operator!=(const base_bidirectional_iterator &it) const { return !operator==(it); }
};

template <typename C, typename Val, typename Next>
requires check_fn<Val, C> &&check_fn<Next, C> class base_forward_iterator
{
  private:
    C current;

  public:
    explicit base_forward_iterator(C c)
        : current(c)
    {
    }

  public:
    C get() { return current; }

    auto &operator*() { return *Val()(current); }
    auto operator&() { return Val()(current); }
    auto operator->() { return Val()(current); }

    base_forward_iterator operator++(int)
    {
        auto old = *this;
        current = Next()(current);
        return old;
    }
    base_forward_iterator &operator++()
    {
        current = Next()(current);
        return *this;
    }
    base_forward_iterator next()
    {
        auto s = *this;
        s.current = Next()(current);
        return s;
    }

    bool operator==(const base_forward_iterator &it) const { return current == it.current; }

    bool operator!=(const base_forward_iterator &it) const { return !operator==(it); }
};

template <typename T> requires must_forward_iterator<T> T previous_iterator(T beg, T target)
{
    cassert(beg != target);
    T prev = beg;
    while (beg != target)
    {
        prev = beg;
        beg++;
    }
    return prev;
}

template <typename T> requires forward_iterator<T> T previous_iterator(T beg, T target)
{
    cassert(beg != target);
    return --target;
}

template <typename T> requires forward_iterator<T> T next_iterator(T end, T target)
{
    cassert(end != target);
    return ++target;
}

template <typename T> requires forward_iterator<T> T go_iterator(T begin, u64 count)
{
    for (int i = 0; i < count; i++)
    {
        begin++;
    }
    return begin;
}

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

template <typename T, typename P> requires forward_iterator<T> T find_if(T beg, T end, P p)
{
    while (beg != end)
    {
        if (p(*beg))
        {
            return beg;
        }
        beg++;
    }
    return beg;
}

} // namespace util