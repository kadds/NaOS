#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "freelibcxx/bit_set.hpp"
#include "freelibcxx/linked_list.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/trace.hpp"
#include "types.hpp"
#include "wait.hpp"
namespace task
{
struct thread_t;
struct process_t;

using signal_num_t = u8;
inline constexpr u64 max_signal_count = 64;

namespace signal
{
enum signal : signal_num_t
{
    sighup = 1,
    sigint,
    sigquit,
    sigill,
    sigtrap,
    sigabrt,
    sigbus,
    sigfpe,
    sigkill,
    siguser1,
    sigsegv,
    siguser2,
    sigpipe,
    sigalarm,
    sigterm,
    sigstkflt,
    sigchild,
    sigcout,
    sigstop,
    signone1,
    signone2,
    signone3,
    signone4,
    signone5,
    sigio = 29,
    sigpower = 30,
    siglimit = 31,
};
}

struct signal_set_t
{
    freelibcxx::bit_set_inplace<max_signal_count> map;
    signal_set_t(u64 mask)
        : map(mask)
    {
    }
    signal_set_t() {}
    signal_set_t &operator+=(signal_num_t num)
    {
        map.set_bit(num);
        return *this;
    };

    signal_set_t &operator-=(signal_num_t num)
    {
        map.reset_bit(num);
        return *this;
    };

    bool operator[](u64 index) { return map.get_bit(index); }
    freelibcxx::bit_set_inplace<max_signal_count> &operator()() { return map; }

    u64 get() { return *map.get_ptr(); }
    void set(u64 mask) { map.set_to(mask); }
};

struct signal_mask_t
{
  private:
    signal_set_t block_bitmap, ignore_bitmap, def_bitmap;

  public:
    signal_mask_t()
    {
        block_bitmap().reset_all();
        ignore_bitmap().reset_all();
        def_bitmap().reset_all();
    }

    signal_set_t &get_block_set() { return block_bitmap; }

    signal_set_t &get_ignore_set() { return ignore_bitmap; }

    signal_set_t &get_valid_set() { return def_bitmap; }

    /// mask signal from [start, start + count) set to mask
    ///
    /// \param start signal number start
    /// \param count signal number start to start + count
    /// \param mask set or clear signal mask
    void block(signal_num_t start, signal_num_t count, bool mask)
    {
        kassert((u16)start + count > max_signal_count, "mask signal number out of range.");
        if (mask)
            block_bitmap().set_all(start, count);
        else
            block_bitmap().reset_all(start, count);
    }

    void ignore(signal_num_t start, signal_num_t count, bool mask)
    {
        kassert((u16)start + count > max_signal_count, "mask signal number out of range.");
        if (mask)
            ignore_bitmap().set_all(start, count);
        else
            ignore_bitmap().reset_all(start, count);
    }

    void valid(signal_num_t start, signal_num_t count, bool mask)
    {
        kassert((u16)start + count > max_signal_count, "mask signal number out of range.");
        if (mask)
            def_bitmap().set_all(start, count);
        else
            def_bitmap().reset_all(start, count);
    }

    bool is_ignore(signal_num_t num) { return ignore_bitmap[num]; }
    bool is_block(signal_num_t num) { return block_bitmap[num]; }
    bool is_valid(signal_num_t num) { return def_bitmap[num]; }
};

struct signal_info_t
{
    i64 number;
    i64 error;
    i64 code;
    process_id pid;
    thread_id tid;
    i64 status;
    signal_info_t() {}
    signal_info_t(i64 number, i64 error, i64 code, process_id pid, thread_id tid, i64 status)
        : number(number)
        , error(error)
        , code(code)
        , pid(pid)
        , tid(tid)
        , status(status)
    {
    }
};

typedef void (*signal_func_t)(process_t *, signal_info_t *info);

extern signal_func_t default_signal_handler[];

namespace sig_action_flag_t
{
enum sig_action_flag_t : flag_t
{
    info = 1,
};
} // namespace sig_action_flag_t

bool sig_condition(u64 data);

struct signal_pack_t
{
  private:
    signal_mask_t masks;

    /// 32 - 63
    freelibcxx::linked_list<signal_info_t> events;

    task::wait_queue_t wait_queue;

  public:
    signal_pack_t()
        : events(memory::KernelCommonAllocatorV)
    {
    }

    void send(process_t *to, signal_num_t num, i64 error, i64 code, i64 status);

    signal_mask_t &get_mask() { return masks; }

    freelibcxx::linked_list<signal_info_t> &get_events() { return events; }

    void wait(signal_info_t *info);
};

} // namespace task
