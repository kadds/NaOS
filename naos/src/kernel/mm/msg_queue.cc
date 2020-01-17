#include "kernel/mm/msg_queue.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/timer.hpp"
#include "kernel/util/hash_map.hpp"

namespace memory
{
u64 lev[] = {2048, max_message_queue_count};
util::id_level_generator<2> *msg_queue_id_generator = nullptr;
util::hash_map<msg_id, message_queue_t *> *msg_hash_map = nullptr;

void init()
{
    msg_queue_id_generator = memory::New<util::id_level_generator<2>>(memory::KernelCommonAllocatorV, lev);
    msg_hash_map = memory::New<util::hash_map<msg_id, message_queue_t *>>(memory::KernelCommonAllocatorV,
                                                                          memory::KernelCommonAllocatorV);
}

message_queue_t *create_msg_queue(u64 maximum_msg_count, u64 maximum_msg_bytes)
{
    if (maximum_msg_bytes > max_message_pack_bytes)
        maximum_msg_bytes = max_message_pack_bytes;
    if (maximum_msg_count > max_message_count)
        maximum_msg_count = max_message_count;
    if (unlikely(msg_queue_id_generator == nullptr))
        init();
    msg_id id = msg_queue_id_generator->next();
    if (id <= 0 || id == util::null_id)
        return nullptr;

    message_queue_t *msgq = memory::New<message_queue_t>(memory::KernelCommonAllocatorV);
    msgq->key = id;
    msg_hash_map->insert(id, msgq);
    return msgq;
}

message_queue_t *get_msg_queue(msg_id id)
{
    if (unlikely(msg_queue_id_generator == nullptr))
        init();
    message_queue_t *q = nullptr;
    msg_hash_map->get(id, &q);
    return q;
}

struct wait_t
{
    message_queue_t *queue;
    msg_type type;
};

bool wait_sender_func(u64 data)
{
    auto *queue = (message_queue_t *)data;
    return queue->msg_count < queue->maximum_msg_count;
}

bool wait_reader_func(u64 data)
{
    auto *w = (wait_t *)data;
    return w->queue->msg_count < w->queue->maximum_msg_count && w->queue->msg_packs.has(w->type);
}

u64 write_msg(message_queue_t *queue, msg_type type, byte *buffer, u64 length, flag_t flags)
{
    if (unlikely(length > max_message_pack_bytes))
        return 0;

    u64 exp;
    do
    {
        if (queue->msg_count >= queue->maximum_msg_count)
        {
            if (flags & msg_flags::no_block)
            {
                return 0;
            }
            else
            {
                task::do_wait(&queue->sender_wait_queue, wait_sender_func, (u64)queue,
                              task::wait_context_type::uninterruptible);
            }
        }
        exp = queue->msg_count;
    } while (!queue->msg_count.compare_exchange_strong(exp, exp + 1, std::memory_order_acquire));

    message_pack_t *msg = (message_pack_t *)memory::KernelBuddyAllocatorV->allocate(1, 0);
    msg->msg_length = length;
    msg->next_msg = nullptr;
    msg->last_msg = nullptr;
    msg->rest_msg = nullptr;
    msg->put_time = timer::get_high_resolution_time();
    msg->type = type;
    u64 len = memory::page_size - offsetof(message_pack_t, buffer);
    const u64 plen = memory::page_size - offsetof(messsage_seg_t, buffer);

    if (len <= length)
    {
        util::memcopy(msg->buffer, buffer, len);
    }
    else
    {
        util::memcopy(msg->buffer, buffer, length);
    }

    messsage_seg_t *last_seg = nullptr;
    while (len < length)
    {
        messsage_seg_t *msgs = (messsage_seg_t *)memory::KernelBuddyAllocatorV->allocate(1, 0);
        u64 mlen = (plen > length - len) ? length - len : plen;
        util::memcopy(msgs->buffer, buffer + len, mlen);
        len += mlen;
        msgs->next = nullptr;
        if (msg->rest_msg == nullptr)
            msg->rest_msg = msgs;
        else
            last_seg->next = msgs;
        last_seg = msgs;
    }
    uctx::RawSpinLockUninterruptibleContext icu(queue->spinlock);

    message_pack_t *root;

    if (queue->msg_packs.get(type, &root))
    {
        root->next_msg = msg;
    }
    else
    {
        root = msg;
        root->last_msg = msg;
        queue->msg_packs.insert(type, root);
    }

    return length;
} // namespace memory

u64 read_msg(message_queue_t *queue, msg_type type, byte *buffer, u64 length, flag_t flags)
{
    message_pack_t *root;

    while (!queue->msg_packs.get(type, &root))
    {
        if (flags & msg_flags::no_block)
        {
            return 0;
        }
        else
        {
            wait_t *wait = memory::New<wait_t>(memory::KernelCommonAllocatorV);
            wait->queue = queue;
            wait->type = type;
            task::do_wait(&queue->receiver_wait_queue, wait_reader_func, (u64)wait,
                          task::wait_context_type::uninterruptible);
        }
    }
    queue->msg_packs.remove_value(type, root);
    message_pack_t *next = root->next_msg;
    if (next)
    {
        queue->msg_packs.insert(type, next);
        next->last_msg = root->last_msg;
    }
    u64 len = memory::page_size - offsetof(message_pack_t, buffer);
    const u64 plen = memory::page_size - offsetof(messsage_seg_t, buffer);
    if (length > root->msg_length)
        length = root->msg_length;

    if (len < length)
    {
        util::memcopy(buffer, root->buffer, len);
    }
    else
    {
        util::memcopy(buffer, root->buffer, length);
        ;
    }

    messsage_seg_t *seg = root->rest_msg;
    while (len < length && seg)
    {
        u64 mlen = plen > length - len ? length - len : plen;
        util::memcopy(buffer + len, seg->buffer, mlen);
        len += mlen;
    }

    queue->msg_count--;
    task::do_wake_up(&queue->sender_wait_queue, 1);
    return length;
}

void close_msg_queue(message_queue_t *q)
{
    if (unlikely(msg_queue_id_generator == nullptr))
        init();
    msg_hash_map->remove(q->key);
}

} // namespace memory
