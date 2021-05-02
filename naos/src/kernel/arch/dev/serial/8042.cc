#include "kernel/arch/dev/serial/8042.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/input/key.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/irq.hpp"
#include "kernel/timer.hpp"
#include "kernel/types.hpp"

namespace arch::device::chip8042
{

constexpr io_port data_port = 0x60;
constexpr io_port cmd_port = 0x64;
constexpr io_port cmd_status_port = 0x64;

void init()
{
    kb_device_class dc;
    if (dev::enum_device(&dc))
    {
        trace::info("8042 keyboard device is available. Loading driver");
        auto driver = memory::New<kb_driver>(memory::KernelCommonAllocatorV);
        auto dev = dev::add_driver(driver);
        if (dev == ::dev::null_num)
        {
            trace::warning("Loading 8042 keyboard driver failed");
            memory::Delete<>(memory::KernelCommonAllocatorV, driver);
        }
        io::attach_request_chain_device(dev, 0, io::chain_number::keyboard);
    }

    mouse_device_class ds;
    if (dev::enum_device(&ds) > 0)
    {
        trace::info("8042 mouse device is available. Loading driver");
        auto driver = memory::New<mouse_driver>(memory::KernelCommonAllocatorV);
        auto dev = dev::add_driver(driver);
        if (dev == ::dev::null_num)
        {
            trace::warning("Loading 8042 mouse driver failed");
            memory::Delete<>(memory::KernelCommonAllocatorV, driver);
        }
        io::attach_request_chain_device(dev, 0, io::chain_number::mouse);
    }
}

void delay()
{
    u64 target = 80 + timer::get_high_resolution_time();
    while (target > timer::get_high_resolution_time())
    {
        for (int i = 0; i < 1000; i++)
        {
        }
    }
}

void flush()
{
    uctx::UninterruptibleContext icu;
    int i = 0;
    while ((io_in8(cmd_status_port) & 0x1) && i < 100)
    {
        delay();
        auto d = io_in8(data_port);
        trace::debug("8042 flush: read ", (void *)(u64)d);
        i++;
    }
}

void wait_for_write()
{
    int i = 0;
    while ((io_in8(cmd_status_port) & 0x2) && i < 100)
    {
        delay();
        i++;
    }
}

void wait_for_read()
{
    int i = 0;

    while (!((io_in8(cmd_status_port)) & 0x1) && i < 100)
    {
        delay();
        i++;
    }
}

// void delay()
// {
//     u64 us = timer::get_high_resolution_time() + 100;
//     while (timer::get_high_resolution_time() < us)
//     {
//     }
// }

::dev::device *kb_device_class::try_scan(int index)
{
    if (index == 0)
    {
        flush();
        // self check failed
        if (!(io_in8(cmd_status_port) & 0b100))
        {
            wait_for_write();
            io_out8(cmd_port, 0xAA);
            wait_for_read();
            if (io_in8(data_port) != 0x55)
            {
                trace::debug("8042 self check failed");
                return nullptr;
            }
        }
        wait_for_write();
        io_out8(cmd_port, 0xAB);
        wait_for_read();
        if (io_in8(data_port) != 0x00)
        {
            trace::debug("keyboard self check failed");
            return nullptr;
        }
        return memory::New<kb_device>(memory::KernelCommonAllocatorV);
    }
    return nullptr;
}

::dev::device *mouse_device_class::try_scan(int index)
{
    if (index == 0)
    {
        flush();
        // self check failed
        if (!(io_in8(cmd_status_port) & 0b100))
        {
            wait_for_write();
            io_out8(cmd_port, 0xAA);
            wait_for_read();
            if (io_in8(data_port) != 0x55)
            {
                trace::debug("8042 self check failed");
                return nullptr;
            }
        }
        wait_for_write();
        io_out8(cmd_port, 0xA9);
        wait_for_read();

        if (io_in8(data_port) != 0x00)
        {
            return nullptr;
        }
        return memory::New<mouse_device>(memory::KernelCommonAllocatorV);
    }
    return nullptr;
}

bool get_key(kb_device *dev, io::keyboard_data *data);

irq::request_result kb_interrupt(const void *regs, u64 extra_data, u64 user_data)
{
    kb_device *dev = (kb_device *)user_data;

    if (unlikely(dev == nullptr))
        return irq::request_result::no_handled;

    u8 data;
    data = io_in8(data_port);
    kb_data_t kb_data;
    kb_data.set(timer::get_high_resolution_time(), data);
    dev->buffer.write_with() = kb_data;

    irq::raise_tasklet(&dev->tasklet);

    return irq::request_result::ok;
}

void kb_tasklet_func(u64 user_data)
{
    kb_device *dev = (kb_device *)user_data;
    if (dev->io_list.size() == 0)
        return;
    io::keyboard_data kbdata;
    if (get_key(dev, &kbdata))
    {
        uctx::RawSpinLockUninterruptibleContext ctx(dev->io_list_lock);
        for (io::keyboard_request_t *it : dev->io_list)
        {
            it->result.get = kbdata;
            it->status.failed_code = 0;
            it->status.io_is_completion = true;
            io::completion(it);
        }
        dev->io_list.clear();
    }
}

bool set_led(u8 s)
{
    wait_for_write();
    io_out8(data_port, 0xED);

    wait_for_read();
    if (io_in8(data_port) != 0xFA)
    {
        trace::warning("set led (no ACK) failed");
        return false;
    }

    wait_for_write();
    io_out8(data_port, s);

    wait_for_read();
    if (io_in8(data_port) != 0xFA)
    {
        trace::warning("set led (no ACK) failed");
        return false;
    }
    return true;
}

void blink_led()
{
    set_led(0b111);
    delay();
    set_led(0b000);
    delay();
    set_led(0b111);
    delay();
    set_led(0b000);
}

u8 get_keyboard_id()
{
    wait_for_write();
    io_out8(data_port, 0xF2);

    wait_for_read();
    if (io_in8(data_port) != 0xFA) // ACK
    {
        trace::warning("get keyboard id failed");
        return 0;
    }
    wait_for_read();

    auto id = io_in8(data_port);
    int i = 0;

    while (((io_in8(cmd_status_port)) & 0x1) && i < 100)
    {
        wait_for_read();
        io_in8(data_port);
        delay();
        i++;
    }

    return id;
}

bool kb_driver::setup(::dev::device *dev)
{
    kb_device *kb_dev = ((kb_device *)dev);
    uctx::UninterruptibleContext icu;

    io_out8(cmd_port, 0x20);
    wait_for_read();
    u8 old_status = io_in8(data_port);
    old_status &= ~0b00010000;
    old_status |= 0b01000001;

    auto id = get_keyboard_id();
    trace::debug("keyboard id is ", (void *)(u64)id);

    kb_dev->id = id;

    blink_led();

    // reset led
    set_led(0);

    kb_dev->led_status = 0;

    io_out8(cmd_port, 0x60);

    wait_for_write();
    io_out8(data_port, old_status);

    flush();

    kb_dev->tasklet.func = kb_tasklet_func;
    kb_dev->tasklet.user_data = reinterpret_cast<u64>(kb_dev);
    irq::add_tasklet(&kb_dev->tasklet);

    APIC::io_entry entry;
    entry.dest_apic_id = APIC::local_ID();
    entry.is_level_trigger_mode = false;
    entry.is_logic_mode = false;
    entry.is_disable = true;
    entry.low_level_polarity = false;
    entry.delivery_mode = APIC::io_entry::mode_t::fixed;

    auto intr = APIC::io_irq_setup(0x1, &entry);
    irq::register_request_func(intr, kb_interrupt, (u64)dev);
    APIC::io_enable(0x1);
    return true;
}

void kb_driver::cleanup(::dev::device *dev)
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (kb_buffer_t *)(dev->get_user_data()));
}

bool get_key(kb_device *dev, io::keyboard_data *data)
{
    kb_data_t kb_data;
    u8 *key = &data->key;
    u64 *timestamp = &data->timestamp;
    bool *release = &data->release;

    while (dev->buffer.read(&kb_data))
    {
        u8 k = kb_data.get_key();
        if (dev->last_prefix_count == 0)
        {
            if (k < 0x59)
            {
                *key = k;
                *release = false;
                *timestamp = kb_data.get_timestamp(timer::get_high_resolution_time());
                return true;
            }
            else if (k > 0x80 && k < 0xD9)
            {
                *key = k - 0x80;
                *release = true;
                *timestamp = kb_data.get_timestamp(timer::get_high_resolution_time());
                return true;
            }
            else if (k == 0xE0 || k == 0xE1)
            {
                dev->last_prefix[dev->last_prefix_count++] = k;
            }
            else
            {
                trace::warning("Unknow scan code ", (void *)(u64)k);
            }
        }
        else if (dev->last_prefix_count == 1)
        {
            if (dev->last_prefix[0] == 0xE0)
            {
                if (k >= 0x90)
                {
                    k -= 0x80;
                    *release = true;
                }
                else
                {
                    *release = false;
                }
                *key = k + 0x50;
                *timestamp = kb_data.get_timestamp(timer::get_high_resolution_time());
                dev->last_prefix_count = 0;
                return true;
            }
            else if (dev->last_prefix[0] == 0xE1)
            {
                dev->last_prefix[dev->last_prefix_count++] = k;
            }
            else
            {
                trace::warning("Unknow scan code ", (void *)(u64)dev->last_prefix[0], ",", (void *)(u64)k);
            }
        }
        else if (dev->last_prefix_count == 2)
        {
            if ((k == 0x45 && dev->last_prefix[1] == 0x1D) || (k == 0xC5 && dev->last_prefix[1] == 0x9D))
            {
                *key = (u8)input::key::pause;
                *release = false;
                *timestamp = kb_data.get_timestamp(timer::get_high_resolution_time());
                dev->last_prefix_count = 0;
                return true;
            }
            else
            {
                trace::warning("Unknow scan code ", (void *)(u64)dev->last_prefix[0], ",",
                               (void *)(u64)dev->last_prefix[1], ",", (void *)(u64)k);
                dev->last_prefix_count = 0;
            }
        }
        else
        {
            trace::warning("Unknow scan code. Unknow prefix. Prefix num: ", dev->last_prefix_count);
        }
    }
    return false;
}

void kb_driver::on_io_request(io::request_t *request)
{
    kassert(request->type == io::chain_number::keyboard, "Error driver state");
    io::keyboard_request_t *req = (io::keyboard_request_t *)request;
    io::status_t &status = req->status;

    kb_device *dev = (kb_device *)req->get_current_device();
    switch (req->cmd_type)
    {
        case io::keyboard_request_t::command::get_key: {
            if (get_key(dev, &req->result.get))
            {
                if (request->poll)
                {
                    status.poll_status = 0;
                }
                else
                {
                    status.failed_code = 0;
                }
                status.io_is_completion = true;
            }
            else
            {
                {
                    uctx::RawSpinLockUninterruptibleContext ctx(dev->io_list_lock);
                    dev->io_list.push_back(std::move(req));
                }
                if (request->poll)
                {
                    status.poll_status = 1;
                }
                status.io_is_completion = false;
            }
            break;
        }
        case io::keyboard_request_t::command::set_repeat: {

            break;
        }
        case io::keyboard_request_t::command::set_led_cas: {
            uctx::UninterruptibleContext icu;

            if (req->cmd_param)
                dev->led_status |= 0b100;
            else
                dev->led_status &= ~(0b100);
            set_led(dev->led_status);
            status.failed_code = 0;
            break;
        }
        case io::keyboard_request_t::command::set_led_scroll: {
            uctx::UninterruptibleContext icu;
            if (req->cmd_param)
                dev->led_status |= 0b001;
            else
                dev->led_status &= ~(0b001);
            set_led(dev->led_status);
            status.failed_code = 0;
            break;
        }
        case io::keyboard_request_t::command::set_led_num: {
            uctx::UninterruptibleContext icu;
            if (req->cmd_param)
                dev->led_status |= 0b010;
            else
                dev->led_status &= ~(0b010);
            set_led(dev->led_status);
            status.failed_code = 0;
            break;
        }
    }
}

//===============================
bool mouse_get(mouse_device *dev, io::mouse_data *data);

irq::request_result mouse_interrupt(const void *regs, u64 extra_data, u64 user_data)
{
    auto dev = (mouse_device *)user_data;
    if (unlikely(dev == nullptr))
        return irq::request_result::no_handled;

    auto data = io_in8(data_port);
    mouse_data_t md;
    md.set(timer::get_high_resolution_time(), data);

    dev->buffer.write_with() = md;
    irq::raise_tasklet(&dev->tasklet);

    return irq::request_result::ok;
}

void mouse_tasklet_func(u64 user_data)
{
    auto dev = (mouse_device *)user_data;
    if (dev->io_list.size() == 0)
        return;
    io::mouse_data io_mouse_data;
    if (mouse_get(dev, &io_mouse_data))
    {
        uctx::RawSpinLockUninterruptibleContext ctx(dev->io_list_lock);
        for (io::mouse_request_t *it : dev->io_list)
        {
            it->result.get = io_mouse_data;
            it->status.failed_code = 0;
            it->status.io_is_completion = true;
            io::completion(it);
        }
        dev->io_list.clear();
    }
}

bool set_mouse_rate(u8 rate)
{
    wait_for_write();
    io_out8(cmd_port, 0xD4);
    wait_for_write();
    io_out8(data_port, 0xF3);
    wait_for_read();
    if (io_in8(data_port) != 0xFA)
    {
        trace::warning("set mouse rate (no ACK) failed");
        return false;
    }
    wait_for_write();
    io_out8(cmd_port, 0xD4);
    wait_for_write();
    io_out8(data_port, rate);
    wait_for_read();
    if (io_in8(data_port) != 0xFA)
    {
        trace::warning("set mouse rate (no ACK) failed");
        return false;
    }
    return true;
}

u8 get_mouse_id()
{
    wait_for_write();
    io_out8(cmd_port, 0xD4);
    wait_for_write();
    io_out8(data_port, 0xF2);
    wait_for_read();
    auto id = io_in8(data_port);
    if (id != 0xFA)
    {
        trace::warning("get mouse id failed");
    }
    else
    {
        wait_for_read();
        id = io_in8(data_port);
        int i = 0;
        while (((io_in8(cmd_status_port)) & 0x1) && i < 100)
        {
            wait_for_read();
            io_in8(data_port);
            delay();
            i++;
        }
        return id;
    }
    return 0;
}

bool mouse_driver::setup(::dev::device *dev)
{
    mouse_device *ms_dev = (mouse_device *)dev;
    wait_for_write();
    io_out8(cmd_port, 0xA8);

    wait_for_write();
    io_out8(cmd_port, 0x20);
    wait_for_read();
    u8 old_status = io_in8(data_port);
    old_status &= ~0b00100000;
    old_status |= 0b00000010;

    // try to enable extensions
    set_mouse_rate(200);
    set_mouse_rate(100);
    set_mouse_rate(80);
    auto id = get_mouse_id();

    if (id == 3)
    {
        set_mouse_rate(200);
        set_mouse_rate(200);
        set_mouse_rate(80);
        id = get_mouse_id();
    }
    ms_dev->id = id;
    trace::debug("mouse id is ", id);

    wait_for_write();
    io_out8(cmd_port, 0x60);
    wait_for_write();
    io_out8(data_port, old_status);

    ms_dev->tasklet.func = mouse_tasklet_func;
    ms_dev->tasklet.user_data = reinterpret_cast<u64>(ms_dev);
    irq::add_tasklet(&ms_dev->tasklet);

    APIC::io_entry entry;
    entry.dest_apic_id = APIC::local_ID();
    entry.is_level_trigger_mode = false;
    entry.is_logic_mode = false;
    entry.is_disable = true;
    entry.low_level_polarity = false;
    entry.delivery_mode = APIC::io_entry::mode_t::fixed;

    auto intr = APIC::io_irq_setup(12, &entry);
    irq::register_request_func(intr, mouse_interrupt, (u64)dev);

    wait_for_write();
    io_out8(cmd_port, 0xD4);

    wait_for_write();
    io_out8(data_port, 0xF4);

    wait_for_read();
    io_in8(data_port); // ACK

    flush();
    APIC::io_enable(12);

    return true;
}

void mouse_driver::cleanup(::dev::device *dev)
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (mouse_buffer_t *)(dev->get_user_data()));
}

bool mouse_get(mouse_device *dev, io::mouse_data *data)
{
    mouse_data_t dt;
    auto &buffer = dev->buffer;
    auto &last_data = dev->last_data;
    auto &last_index = dev->last_index;

    while (buffer.read(&dt))
    {
        switch (last_index)
        {
            case 0:
                last_data[last_index++] = dt.data;
                if (!(dt.data & 0b1000))
                {
                    trace::warning("Unknown mouse data.");
                    last_index = 0;
                }
                break;
            case 1:
                last_data[last_index++] = dt.data;
                break;
            case 2:
                last_data[last_index++] = dt.data;
                if (dev->id == 0)
                {
                    data->movement_x = (u16)last_data[1] - ((last_data[0] << 4) & 0x100);
                    data->movement_y = (u16)last_data[2] - ((last_data[0] << 3) & 0x100);

                    data->down_x = last_data[0] & 0b1;
                    data->down_y = last_data[0] & 0b10;
                    data->down_z = last_data[0] & 0b100;
                    data->movement_z = 0;

                    data->timestamp = dt.get_timestamp(timer::get_high_resolution_time());
                    last_index = 0;
                    return true;
                }
                break;
            case 3:
                last_data[last_index] = dt.data;
                last_index = 0;

                data->movement_x = (u16)last_data[1] - ((last_data[0] << 4) & 0x100);
                data->movement_y = (u16)last_data[2] - ((last_data[0] << 3) & 0x100);
                data->movement_z = 8 - ((8 - last_data[3]) & 0xF);

                data->down_x = last_data[0] & 0b1;
                data->down_y = last_data[0] & 0b10;
                data->down_z = last_data[0] & 0b100;
                data->down_a = last_data[3] & 0b10000;
                data->down_b = last_data[3] & 0b100000;

                data->timestamp = dt.get_timestamp(timer::get_high_resolution_time());
                last_index = 0;
                return true;
        }
    }
    return false;
}

void mouse_driver::on_io_request(io::request_t *request)
{
    kassert(request->type == io::chain_number::mouse, "Error driver state");
    io::mouse_request_t *req = (io::mouse_request_t *)request;
    io::status_t &status = req->status;

    mouse_device *dev = (mouse_device *)req->get_current_device();

    switch (req->cmd_type)
    {
        case io::mouse_request_t::command::get: {
            if (mouse_get(dev, &req->result.get))
            {
                if (request->poll)
                {
                    status.poll_status = 0;
                }
                else
                {
                    status.failed_code = 0;
                }
                status.io_is_completion = true;
            }
            else
            {
                {
                    uctx::RawSpinLockUninterruptibleContext ctx(dev->io_list_lock);
                    dev->io_list.push_back(std::move(req));
                }
                if (request->poll)
                {
                    status.poll_status = 1;
                }
                status.io_is_completion = false;
            }
        }
        break;
        case io::mouse_request_t::command::set_speed: {
            /// TODO: set speed for mouse
            status.failed_code = 0;
            break;
        }
        default:
            break;
    }
}

} // namespace arch::device::chip8042
