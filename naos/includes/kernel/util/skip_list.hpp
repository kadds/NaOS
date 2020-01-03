#pragma once
#include "common.hpp"
#include "linked_list.hpp"
namespace util
{
template <typename E, typename cmp_func> class skip_list
{
  public:
    struct node_t
    {
        E element;
        node_t *next;
        node_t *parent;
        node_t *child;
    };

  private:
    node_t *root_array;

  public:
    void insert();
};
} // namespace util