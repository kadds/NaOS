#pragma once
#include "../mm/new.hpp"
#include "common.hpp"
namespace util
{
class ring_buffer
{
  public:
    struct trunk
    {
        byte *buffer;
        u64 offset;
        u64 size;
        trunk *next;
    };
    enum class strategy
    {
        no_wait,
        discard,
        rewrite,
    };

  private:
    u64 trunk_size;
    u64 max_trunk_count;
    u64 count;

    memory::IAllocator *node_allocator;
    memory::IAllocator *allocator;
    strategy full_strategy;

    trunk *read_trunk, *write_trunk, *free_trunk;

  public:
    ring_buffer(u64 trunk_size, u64 max_trunk_count, strategy full_strategy, memory::IAllocator *list_node_allocator,
                memory::IAllocator *trunk_allocator);

    u64 write(const byte *buffer, u64 size);

    byte *read_buffer(u64 *read_size);

  private:
    trunk *new_trunk();
    void delete_trunk(trunk *t);
};
} // namespace util
