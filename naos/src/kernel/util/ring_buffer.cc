#include "kernel/util/ring_buffer.hpp"
#include "kernel/util/memory.hpp"

namespace util
{
ring_buffer::ring_buffer(u64 trunk_size, u64 max_trunk_count, strategy full_strategy,
                         memory::IAllocator *list_node_allocator, memory::IAllocator *trunk_allocator)
    : trunk_size(trunk_size)

    , max_trunk_count(max_trunk_count)
    , count(0)
    , node_allocator(list_node_allocator)
    , allocator(trunk_allocator)
    , full_strategy(full_strategy)
    , free_trunk(nullptr)
{
    trunk *tk = new_trunk();
    read_trunk = tk;
    write_trunk = tk;
    count++;
    free_trunk = nullptr;
}

u64 ring_buffer::write(const byte *buffer, u64 size)
{
    u64 cur_write = 0;
    while (cur_write < size)
    {
        if (unlikely(write_trunk->size == trunk_size))
        {
            // new trunk
            if (unlikely(count == max_trunk_count))
            {
                if (full_strategy == strategy::discard)
                {
                    return size;
                }
                else if (full_strategy == strategy::rewrite)
                {
                }
                else if (full_strategy == strategy::no_wait)
                {
                    return cur_write;
                }
            }
            if (free_trunk == nullptr)
            {
                free_trunk = new_trunk();
            }
            free_trunk->next = nullptr;
            free_trunk->size = 0;
            free_trunk->offset = 0;
            write_trunk->next = free_trunk;
            free_trunk = free_trunk->next;
            write_trunk = write_trunk->next;
        }
        u64 rest = trunk_size - write_trunk->size;
        if (size - cur_write < rest)
        {
            rest = size - cur_write;
        }
        memcopy(write_trunk->buffer + write_trunk->size, buffer, rest);
        write_trunk->size += rest;
        cur_write += rest;
    }
    if (read_trunk == nullptr)
    {
        read_trunk = write_trunk;
    }
    return cur_write;
}

byte *ring_buffer::read_buffer(u64 *size)
{
    if (unlikely(read_trunk == nullptr))
    {
        *size = 0;
        return nullptr;
    }
    byte *buffer;
    *size = read_trunk->size - read_trunk->offset;
    buffer = read_trunk->buffer + read_trunk->offset;

    read_trunk->offset = read_trunk->size;

    if (read_trunk->size == trunk_size)
    {
        auto t = read_trunk;
        read_trunk = read_trunk->next;
        t->next = free_trunk;
        free_trunk = t;
    }
    return buffer;
}

ring_buffer::trunk *ring_buffer::new_trunk()
{
    trunk *tk = (trunk *)node_allocator->allocate(sizeof(trunk), alignof(trunk));
    tk->buffer = (byte *)allocator->allocate(trunk_size, 1);
    count++;
    return tk;
}

void ring_buffer::delete_trunk(ring_buffer::trunk *t)
{
    count--;
    allocator->deallocate(t->buffer);
    node_allocator->deallocate(t);
}

} // namespace util
