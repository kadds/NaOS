#pragma once
#include "../mutex.hpp"
#include "../types.hpp"
#include "../util/hash_map.hpp"
#include "../util/id_generator.hpp"
#include "../util/linked_list.hpp"
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
    timeclock::microsecond_t put_time;
    msg_type type;
    u64 msg_length;
    messsage_seg_t *rest_msg;
    byte buffer[1];
};

using msg_pack_list_t = util::linked_list<message_pack_t *>;
using msg_pack_hash_map_t = util::hash_map<msg_type, msg_pack_list_t *>;

inline constexpr u64 max_message_queue_count = 65536;
inline constexpr u64 max_message_count = 1024;
inline constexpr u64 max_message_pack_bytes = 1ul << 22;

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
    task::wait_queue_t receiver_wait_queue;
    task::wait_queue_t sender_wait_queue;
    msg_pack_hash_map_t msg_packs;
    lock::spinlock_t spinlock;
    volatile bool close;

    message_queue_t()
        : msg_count(0)
        , msg_packs(memory::KernelCommonAllocatorV)
        , close(false){};
};

namespace msg_flags
{
enum
{
    no_block = 1,
    no_block_other = 2,
};
} // namespace msg_flags

message_queue_t *create_msg_queue(u64 maximum_msg_count = max_message_count,
                                  u64 maximum_msg_bytes = max_message_pack_bytes);
i64 write_msg(message_queue_t *queue, msg_type type, const byte *buffer, u64 length, flag_t flags);
message_queue_t *get_msg_queue(msg_id msg_id);
i64 read_msg(message_queue_t *queue, msg_type type, byte *buffer, u64 length, flag_t flags);
bool close_msg_queue(message_queue_t *q);

void msg_queue_init();

} // namespace memory
