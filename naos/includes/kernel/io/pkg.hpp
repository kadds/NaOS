#pragma once
#include "../dev/device.hpp"
#include "../dev/driver.hpp"
#include "../wait.hpp"
#include "common.hpp"
namespace io
{
enum class completion_result_t
{
    conti, ///< continue stack uptrace
    inter, ///< interrupt completion call
};

namespace chain_number
{
enum : u32
{
    disk = 1,
    mouse = 2,
    keyboard = 3,
    touch = 4,
    network = 10,
};
} // namespace chain_number

using request_chain_number_t = u32;

struct status_t
{
    u8 poll_status;
    u8 failed_code;

    bool io_is_completion;
};

struct mouse_data
{
    i16 movement_x, movement_y, movement_z;
    struct
    {
        bool down_x : 1;
        bool down_y : 1;
        bool down_z : 1;
        bool down_a : 1;
        bool down_b : 1;
    };
    u64 timestamp;
};
struct mouse_result_t
{
    union
    {
        mouse_data get;
    };
};

struct request_t;

typedef completion_result_t (*completion_stack_func)(request_t *req, u64 user_data);
typedef void (*completion_result_func_t)(request_t *req, bool intr, u64 user_data);

struct io_stack_t
{
    dev::num_t dev_num;
    completion_stack_func completion_callback;
    u64 completion_user_data;
};

struct request_inner_t
{
    u32 cur_stack_index;
    u32 stack_size;
    io_stack_t *io_stack;
    dev::num_t get_current_dev() { return io_stack[cur_stack_index].dev_num; }
    dev::device *get_current_device() { return dev::get_device(io_stack[cur_stack_index].dev_num); }
    void set_current_device_completion_callback(completion_stack_func func, u64 user_data)
    {
        io_stack[cur_stack_index].completion_user_data = user_data;
        io_stack[cur_stack_index].completion_callback = func;
    }
};

struct request_t
{
    request_chain_number_t type;
    bool poll;
    completion_result_func_t final_completion_func;
    u64 completion_user_data;
    status_t status;
    /// --- for inner use
    request_inner_t inner;

    dev::device *get_current_device() { return inner.get_current_device(); }
};

struct mouse_request_t : request_t
{
    enum class command
    {
        set_speed,
        get,
    } cmd_type;
    u64 cmd_param;
    mouse_result_t result;
};

struct keyboard_data
{
    u8 key;
    bool release;
    u64 timestamp;
};

struct keyboard_result_t
{
    union
    {
        keyboard_data get;
    };
};

struct keyboard_request_t : request_t
{
    enum class command
    {
        set_led_cas,
        set_led_scroll,
        set_led_num,
        set_repeat,
        get_key,
    } cmd_type;
    u64 cmd_param;
    keyboard_result_t result;
};

struct disk_result_t
{
};

struct disk_request_t : request_t
{
    enum class command
    {
        write,
        read,
    } cmd_type;

    byte *buffer;
    u64 buffer_start;
    u64 buffer_length;
    disk_result_t result;
};

} // namespace io
