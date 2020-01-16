#pragma once
#include "../util/id_generator.hpp"
#include "common.hpp"
#include "mm/new.hpp"
#include "types.hpp"
#include "wait.hpp"

namespace memory
{
using msg_id = i64;

struct message_pack_t
{
    u64 msg_length;
    byte *buffer;
    message_pack_t *next;
};

struct message_queue_t
{
    handle_t key;
    enum class mode_t
    {
        none = 0,
    };
    mode_t mode;
    task::wait_queue receiver_wait_queue;
    task::wait_queue sender_wait_queue;
    time::microsecond_t last_read;
    time::microsecond_t last_write;

    message_queue_t()
        : receiver_wait_queue(memory::KernelCommonAllocatorV)
        , sender_wait_queue(memory::KernelCommonAllocatorV){};
};

message_queue_t *create_msg_queue();
msg_id write_msg(message_queue_t *queue, msg_id msg_id, byte *buffer, u64 length);
message_queue_t *get_msg_queue();
void read_msg(message_queue_t *queue, msg_id msg_id, byte *buffer, u64 length);

u64 msg_ctl();

} // namespace memory
