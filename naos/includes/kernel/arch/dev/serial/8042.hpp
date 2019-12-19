#pragma once
#include "../../../dev/device.hpp"
#include "../../../dev/driver.hpp"
#include "../../../util/circular_buffer.hpp"

namespace arch::device::chip8042
{
struct kb_device_class : public ::dev::device_class
{
    ::dev::device *try_scan(int index) override;
};

struct mouse_device_class : public ::dev::device_class
{
    ::dev::device *try_scan(int index) override;
};

struct kb_data_t
{
    u8 timestamp_low;
    u8 timestamp_mid;
    u8 timestamp_high;
    u8 data;

    u64 get_timestamp(u64 current_timestamp)
    {
        return ~((u64)timestamp_low | ((u64)(timestamp_mid) << 8) | ((u64)(timestamp_high) << 16)) & current_timestamp;
    }

    u8 get_key() { return data; }

    void set(u64 timestamp, u8 key)
    {
        timestamp_low = timestamp & 0xFF;
        timestamp_mid = (timestamp >> 8) & 0xFF;
        timestamp_high = (timestamp >> 16) & 0xFF;
        data = key;
    }
};

constexpr u64 kb_cache_count = 32;
using kb_buffer_t = util::circular_buffer<kb_data_t>;

class kb_device : public ::dev::device
{
  public:
    kb_buffer_t buffer;
    kb_device()
        : device(::dev::type::chr, "8042keyboard")
        , buffer(memory::KernelCommonAllocatorV, kb_cache_count)
    {
    }
};

class kb_driver : public ::dev::driver
{
  public:
    kb_driver()
        : dev::driver(::dev::type::chr, "8042keyboard")
    {
    }

    bool setup(::dev::device *dev) override;
    void cleanup(::dev::device *dev) override;
    void on_io_request(io::request_t *request) override;
    u64 get_key_data(::dev::device *dev, kb_data_t *data, u64 max_len);
};

constexpr u64 mouse_cache_count = 128;

struct mouse_data_t
{
    u8 timestamp_low;
    u8 timestamp_mid;
    u8 timestamp_high;
    u8 x, y, z;
    struct
    {
        bool x_down : 1;
        bool y_down : 1;
        bool z_down : 1;
        bool a_down : 1;
        bool b_down : 1;
        bool x_positive : 1;
        bool y_positive : 1;
    };
};

using mouse_buffer_t = util::circular_buffer<mouse_data_t>;

class mouse_device : public ::dev::device
{
  public:
    mouse_buffer_t buffer;
    mouse_device()
        : device(::dev::type::chr, "8042mouse")
        , buffer(memory::KernelCommonAllocatorV, mouse_cache_count)
    {
    }
};

class mouse_driver : public ::dev::driver
{
  public:
    mouse_driver()
        : dev::driver(::dev::type::chr, "8042mouse")
    {
    }
    bool setup(::dev::device *dev) override;
    void cleanup(::dev::device *dev) override;
    void on_io_request(io::request_t *request) override;

    u64 get_mouse_data(::dev::device *dev, kb_data_t *data, u64 max_len);
};

void init();

} // namespace arch::device::chip8042
