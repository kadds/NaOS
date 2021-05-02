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

template <typename T, typename E> concept forward_iterator = requires(T t) { t.next(); };

template <typename T, typename E> concept bidirectional_iterator = requires(T t)
{
    t.next();
    t.prev();
};
template <typename T, typename E> concept random_iterator = requires(T t) { t.at(1); };

} // namespace util