#pragma once
#include "../dev/device.hpp"
#include "../dev/driver.hpp"
#include "../wait.hpp"
#include "common.hpp"
namespace io
{
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

struct result_t
{
    u64 poll_status;
};

struct mouse_result_t : result_t
{
    u16 movement_x, movement_y, movement_z;
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

struct request_t
{
    request_chain_number_t type;
    u32 cur_stack_index;
    dev::num_t *dev_stack;
    bool poll;
    dev::num_t get_current_dev() { return dev_stack[cur_stack_index]; }
    dev::device *get_current_device() { return dev::get_device(dev_stack[cur_stack_index]); }
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

struct keyboard_result_t : result_t
{
    union
    {
        struct
        {
            u8 key;
            bool release;
            u64 timestamp;
        } get;
        struct
        {
            bool ok;
        } set;
    } cmd_type;
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

struct disk_result_t : result_t
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
