#include "kernel/mm/msg_queue.hpp"
#include "kernel/mm/buddy.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/timer.hpp"
#include "kernel/util/hash_map.hpp"

namespace memory
{
u64 lev[] = {2048, max_message_queue_count};
util::seq_generator *msg_queue_id_generator = nullptr;
util::hash_map<msg_id, message_queue_t *> *msg_hash_map = nullptr;
lock::rw_lock_t msg_queue_lock;

void msg_queue_init()
{
    msg_queue_id_generator = memory::New<util::seq_generator>(memory::KernelCommonAllocatorV, 1, 1);

    msg_hash_map = memory::New<util::hash_map<msg_id, message_queue_t *>>(memory::KernelCommonAllocatorV,
                                                                          memory::KernelCommonAllocatorV, 7, 100);
}

message_queue_t *create_msg_queue(u64 maximum_msg_count, u64 maximum_msg_bytes)
{
    if (maximum_msg_bytes > max_message_pack_bytes)
        maximum_msg_bytes = max_message_pack_bytes;
    if (maximum_msg_count > max_message_count)
        maximum_msg_count = max_message_count;
    msg_id id = msg_queue_id_generator->next();
    if (id <= 0 || id == util::null_id)
        return nullptr;

    message_queue_t *msgq = memory::New<message_queue_t>(memory::KernelCommonAllocatorV);
    msgq->key = id;
    msgq->maximum_msg_count = maximum_msg_count;
    msgq->maximum_msg_bytes = maximum_msg_bytes;
    uctx::RawWriteLockUninterruptibleContext icu(msg_queue_lock);
    msg_hash_map->insert(id, msgq);
    return msgq;
}

void delete_msg_queue(message_queue_t *q)
{
    uctx::RawWriteLockUninterruptibleContext icu(msg_queue_lock);
    msg_hash_map->remove(q->key);
    // msg_queue_id_generator->collect(q->key);
    memory::Delete<>(memory::KernelCommonAllocatorV, q);
}

message_queue_t *get_msg_queue(msg_id id)
{
    uctx::RawReadLockUninterruptibleContext icu(msg_queue_lock);
    message_queue_t *q = nullptr;
    msg_hash_map->get(id, &q);
    return q;
}

struct wait_t
{
    message_queue_t *queue;
    msg_type type;
    flag_t flags;
};

bool wait_sender_func(u64 data)
{
    auto *queue = (message_queue_t *)data;
    return queue->msg_count < queue->maximum_msg_count || queue->close;
}

u64 write_msg_data(message_pack_t *msg, const byte *buffer, u64 length)
{
    msg->msg_length = length;
    msg->rest_msg = nullptr;
    msg->put_time = timer::get_high_resolution_time();
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
        util::memcopy(msgs->buffer + len, buffer + len, mlen);
        len += mlen;
        msgs->next = nullptr;
        if (msg->rest_msg == nullptr)
            msg->rest_msg = msgs;
        else
            last_seg->next = msgs;
        last_seg = msgs;
    }
    return length;
}

bool write_for_write(message_queue_t *queue, flag_t flags)
{
    while (queue->msg_count >= queue->maximum_msg_count) // full
    {
        if (flags & msg_flags::no_block)
            return false;
        queue->sender_wait_queue.do_wait(wait_sender_func, (u64)queue);
        if (unlikely(queue->close))
            return false;
    }
    return true;
}

i64 write_msg(message_queue_t *queue, msg_type type, const byte *buffer, u64 length, flag_t flags)
{
    if (unlikely(length > max_message_pack_bytes) || queue->close)
        return 0;
    if (!write_for_write(queue, flags))
        return -1;
    message_pack_t *msg = (message_pack_t *)memory::KernelBuddyAllocatorV->allocate(1, 0);
    msg->type = type;
    write_msg_data(msg, buffer, length);

    {
        msg_pack_list_t *list;
        uctx::RawSpinLockUninterruptibleContext icu(queue->spinlock);
        if (!write_for_write(queue, flags))
            return -1;

        if (!queue->msg_packs.get(type, &list))
        {
            list = memory::New<msg_pack_list_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
            queue->msg_packs.insert(type, list);
        }
        list->push_back(msg);
        queue->msg_count++;
    }
    queue->receiver_wait_queue.do_wake_up();
    return length;
}

bool wait_reader_func(u64 data)
{
    auto *w = reinterpret_cast<wait_t *>(data);
    msg_pack_list_t *pack_list;
    uctx::RawSpinLockUninterruptibleContext icu(w->queue->spinlock);
    if (!w->queue->msg_packs.get(w->type, &pack_list) || pack_list->empty())
    {
        if (w->queue->close && w->queue->msg_count == 0)
        {
            return true;
        }
        if ((w->flags & msg_flags::no_block_other) && w->queue->msg_count > 0)
        {
            return true;
        }
        return false;
    }
    return true;
}

u64 read_msg_data(message_pack_t *msg, byte *buffer, u64 length)
{
    u64 len = memory::page_size - offsetof(message_pack_t, buffer);
    const u64 plen = memory::page_size - offsetof(messsage_seg_t, buffer);
    if (length > msg->msg_length)
        length = msg->msg_length;

    if (len < length)
    {
        util::memcopy(buffer, msg->buffer, len);
    }
    else
    {
        util::memcopy(buffer, msg->buffer, length);
    }

    messsage_seg_t *seg = msg->rest_msg;
    memory::Delete<>(memory::KernelBuddyAllocatorV, msg);
    while (len < length && seg)
    {
        u64 mlen = plen > length - len ? length - len : plen;
        util::memcopy(buffer + len, seg->buffer, mlen);
        len += mlen;
        auto next = seg->next;
        memory::Delete<>(memory::KernelBuddyAllocatorV, seg);
        seg = next;
    }
    return length;
}

i64 read_msg(message_queue_t *queue, msg_type type, byte *buffer, u64 length, flag_t flags)
{
    message_pack_t *msg;
    msg_pack_list_t *msg_pack_list = nullptr;
    for (;;)
    {
        if (queue->close && queue->msg_count == 0)
        {
            delete_msg_queue(queue);
            return -1;
        }

        uctx::RawSpinLockUninterruptibleController icu(queue->spinlock);
        icu.begin();
        if (!queue->msg_packs.get(type, &msg_pack_list) || msg_pack_list->empty())
        {
            icu.end();
            if (flags & msg_flags::no_block)
                return -1;
            if (flags & msg_flags::no_block_other)
                if (queue->msg_count > 0)
                    return -2;

            wait_t *wait = memory::New<wait_t>(memory::KernelCommonAllocatorV);
            wait->queue = queue;
            wait->type = type;
            wait->flags = flags;
            queue->receiver_wait_queue.do_wait(wait_reader_func, (u64)wait);
            memory::Delete<>(memory::KernelCommonAllocatorV, wait);
        }
        else
        {
            msg = msg_pack_list->pop_front();
            queue->msg_count--;
            icu.end();
            break;
        }
    }

    read_msg_data(msg, buffer, length);

    queue->sender_wait_queue.do_wake_up();
    return length;
}

bool close_msg_queue(message_queue_t *queue)
{
    uctx::RawSpinLockUninterruptibleController icu(queue->spinlock);
    icu.begin();
    queue->close = true;
    queue->sender_wait_queue.do_wake_up();
    queue->receiver_wait_queue.do_wake_up();
    if (queue->msg_count == 0)
    {
        icu.end();
        delete_msg_queue(queue);
    }
    else
    {
        icu.end();
    }
    return true;
}

} // namespace memory
