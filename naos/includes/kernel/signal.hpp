#pragma once
#include "common.hpp"
#include "util/bit_set.hpp"
namespace task
{
struct thread_t;

using signal_num_t = u8;
constexpr u64 max_signal_count = 64;

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
    sigpower = 30,
    siglimit = 31,
};

struct signal_mask_t
{
  private:
    util::bit_set_inplace<max_signal_count> block_bitmap, ignore_bitmap;

  public:
    signal_mask_t()
    {
        block_bitmap.clean_all();
        ignore_bitmap.clean_all();
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
            block_bitmap.set_all(start, count);
        else
            block_bitmap.clean_all(start, count);
    }

    void ignore(signal_num_t start, signal_num_t count, bool mask)
    {
        kassert((u16)start + count > max_signal_count, "mask signal number out of range.");
        if (mask)
            ignore_bitmap.set_all(start, count);
        else
            ignore_bitmap.clean_all(start, count);
    }

    bool is_ignore(signal_num_t num) { return ignore_bitmap.get(num); }
    bool is_block(signal_num_t num) { return block_bitmap.get(num); }
};

typedef void (*signal_func_t)(signal_num_t num);

extern signal_func_t default_signal_handler[];

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
    };

    action actions[max_signal_count];

    void set_action(signal_num_t num, signal_func_t func) { actions[num].handler = func; }

    void clean_action(signal_num_t num) { actions[num].handler = default_signal_handler[num]; }

    signal_func_t get_action(signal_num_t num) { return actions[num].handler; }

    signal_actions_t();
};

struct signal_pack_t
{
  private:
    util::bit_set_inplace<max_signal_count> pending;
    signal_mask_t masks;

  public:
    void set(signal_num_t num);

    void dispatch(signal_actions_t *actions);

    signal_mask_t &get_mask() { return masks; }
};

ExportC void do_signal();

} // namespace task
