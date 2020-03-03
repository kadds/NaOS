#pragma once
#include "arch/task.hpp"
#include "common.hpp"
#include "types.hpp"
#include "util/array.hpp"
#include "util/bit_set.hpp"
#include "util/linked_list.hpp"
namespace task
{
struct thread_t;

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
    util::bit_set_inplace<max_signal_count> map;
    signal_set_t(u64 mask)
        : map(mask)
    {
    }
    signal_set_t() {}
    signal_set_t &operator+=(signal_num_t num)
    {
        map.set(num);
        return *this;
    };

    signal_set_t &operator-=(signal_num_t num)
    {
        map.clean(num);
        return *this;
    };

    bool operator[](u64 index) { return map.get(index); }
    util::bit_set_inplace<max_signal_count> &operator()() { return map; }
};

struct signal_mask_t
{
  private:
    signal_set_t block_bitmap, ignore_bitmap;

  public:
    signal_mask_t()
    {
        block_bitmap().clean_all();
        ignore_bitmap().clean_all();
    }
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
            block_bitmap().clean_all(start, count);
    }

    void ignore(signal_num_t start, signal_num_t count, bool mask)
    {
        kassert((u16)start + count > max_signal_count, "mask signal number out of range.");
        if (mask)
            ignore_bitmap().set_all(start, count);
        else
            ignore_bitmap().clean_all(start, count);
    }

    bool is_ignore(signal_num_t num) { return ignore_bitmap[num]; }
    bool is_block(signal_num_t num) { return block_bitmap[num]; }
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

typedef void (*signal_func_t)(signal_num_t num, signal_info_t *info);

extern signal_func_t default_signal_handler[];

namespace sig_action_flag_t
{
enum sig_action_flag_t : flag_t
{
    info = 1,
};
} // namespace sig_action_flag_t

struct signal_actions_t
{
    enum signal_type
    {
        type_post_main_thread = 0,
        type_post_any = 1,
        type_post_self = 2,
    };

    struct signal_pack
    {
        thread_t *from;
        u64 user_data;
        signal_pack(thread_t *from, u64 user_data)
            : from(from)
            , user_data(user_data)
        {
        }
    };

    struct action
    {
        signal_func_t handler;
        signal_set_t ignore_bitmap;
        flag_t flags;
    };

  private:
    action actions[max_signal_count];

  public:
    void set_action(signal_num_t num, signal_func_t handler, signal_set_t set, flag_t flags);

    void clean_all();

    void clean_action(signal_num_t num) { actions[num].handler = default_signal_handler[num]; }

    action &get_action(signal_num_t num) { return actions[num]; }

    signal_actions_t();
};

struct signal_pack_t
{
  private:
    signal_mask_t masks;
    bool sig_pending = false;
    bool in_signal = false;
    signal_num_t current_signal_sent;
    void *signal_stack = nullptr;

    arch::task::userland_code_context context;
    /// 0 - 31 signal
    signal_set_t pending_bitmap;

    /// 32 - 63
    util::linked_list<signal_info_t> events;

  public:
    signal_pack_t()
        : events(memory::KernelCommonAllocatorV)
    {
    }

    void set(signal_num_t num, i64 error, i64 code, i64 status);

    void dispatch(signal_actions_t *actions);

    void user_return();

    signal_mask_t &get_mask() { return masks; }

    bool is_set() { return sig_pending; }
    bool is_in_signal() { return in_signal; }
    void set_in_signal(bool in) { in_signal = in; }
};
void signal_return();
void do_signal();

} // namespace task
