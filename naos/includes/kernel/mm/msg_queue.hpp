#pragma once
#include "../types.hpp"
#include "../util/hash_map.hpp"
#include "../util/id_generator.hpp"
#include "../wait.hpp"
#include "common.hpp"
#include "new.hpp"
#include <atomic>

namespace memory
{
using msg_id = u64;
using msg_type = u64;

struct messsage_seg_t
{
    messsage_seg_t *next;
    byte buffer[1];
};

struct message_pack_t
{
    time::microsecond_t put_time;
    message_pack_t *next_msg;
    message_pack_t *last_msg;
    msg_type type;
    u64 msg_length;
    messsage_seg_t *rest_msg;
    byte buffer[1];
};

using msg_pack_hash_map_t = util::hash_map<msg_type, message_pack_t *>;

inline constexpr u64 max_message_queue_count = 65536;
inline constexpr u64 max_message_count = 8192;
inline constexpr u64 max_message_pack_bytes = 1ul << 30;

struct message_queue_t
{
    msg_id key;
    enum class mode_t
    {
        none = 0,
    };
    mode_t mode;
    std::atomic_uint64_t msg_count;
    u64 maximum_msg_count;
    u64 maximum_msg_bytes;
    task::wait_queue receiver_wait_queue;
    task::wait_queue sender_wait_queue;
    msg_pack_hash_map_t msg_packs;

    lock::spinlock_t spinlock;

    message_queue_t()
        : msg_count(0)
        , receiver_wait_queue(memory::KernelCommonAllocatorV)
        , sender_wait_queue(memory::KernelCommonAllocatorV)
        , msg_packs(memory::KernelCommonAllocatorV){};
};

namespace msg_flags
{
enum
{
    no_block = 1,
};
} // namespace msg_flags

message_queue_t *create_msg_queue(u64 maximum_msg_count = max_message_count,
                                  u64 maximum_msg_bytes = max_message_pack_bytes);
u64 write_msg(message_queue_t *queue, msg_type type, byte *buffer, u64 length, flag_t flags);
message_queue_t *get_msg_queue(msg_id msg_id);
u64 read_msg(message_queue_t *queue, msg_type type, byte *buffer, u64 length, flag_t flags);
void close_msg_queue(message_queue_t *q);

} // namespace memory
