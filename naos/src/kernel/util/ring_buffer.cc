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
{
    trunk *tk = new_trunk();
    read_trunk = tk;
    write_trunk = tk;
}

u64 ring_buffer::write(const byte *buffer, u64 size)
{
    u64 cur_write = 0;
    while (cur_write < size)
    {
        u64 ws = write_trunk->size;
        if (unlikely(ws == trunk_size))
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
            trunk *tk = write_trunk->next;
            if (tk == nullptr)
            {
                tk = new_trunk();
                write_trunk->next = tk;
            }
            write_trunk = tk;
            ws = tk->size;
            tk->size = 0;
            tk->offset = 0;
        }
        u64 rest = trunk_size - ws;
        if (size - cur_write < rest)
        {
            rest = size - cur_write;
        }
        memcopy(write_trunk->buffer + ws, buffer, rest);
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
    u64 off = read_trunk->offset;
    *size = read_trunk->size - off;
    buffer = read_trunk->buffer + off;

    read_trunk->offset = *size + off;

    if (read_trunk->offset == trunk_size)
    {
        auto cur = read_trunk;
        read_trunk = read_trunk->next;

        trunk *ft;
        do
        {
            ft = write_trunk->next;
            cur->next = write_trunk->next;
        } while (unlikely(__sync_val_compare_and_swap(&write_trunk->next, ft, cur) != ft));
    }
    return buffer;
}

ring_buffer::trunk *ring_buffer::new_trunk()
{
    trunk *tk = (trunk *)node_allocator->allocate(sizeof(trunk), alignof(trunk));
    tk->buffer = (byte *)allocator->allocate(trunk_size, 1);
    tk->next = nullptr;
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
