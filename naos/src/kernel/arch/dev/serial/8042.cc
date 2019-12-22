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

io_port data_port = 0x60;
io_port cmd_port = 0x64;
io_port cmd_status_port = 0x64;

void init()
{
    kb_device_class dc;
    if (dev::enum_device(&dc))
    {
        trace::info("Find 8042 keyboard device. Loading driver.");
        auto driver = memory::New<kb_driver>(memory::KernelCommonAllocatorV);
        auto dev = dev::add_driver(driver);
        if (dev == ::dev::null_num)
        {
            trace::warning("Load 8042 keyboard driver failed.");
            memory::Delete<>(memory::KernelCommonAllocatorV, driver);
        }
        io::attach_request_chain_device(dev, 0, io::chain_number::keyboard);
    }

    mouse_device_class ds;
    if (dev::enum_device(&ds) > 0)
    {
        trace::info("Find 8042 mouse device. Loading driver.");
        auto driver = memory::New<mouse_driver>(memory::KernelCommonAllocatorV);
        auto dev = dev::add_driver(driver);
        if (dev == ::dev::null_num)
        {
            trace::warning("Load 8042 mouse driver failed.");
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
    uctx::UnInterruptableContext icu;
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
                trace::debug("8042 self check failed.");
                return nullptr;
            }
        }
        wait_for_write();
        io_out8(cmd_port, 0xAB);
        wait_for_read();
        if (io_in8(data_port) != 0x00)
        {
            trace::debug("keyboard self check failed.");
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
                trace::debug("8042 self check failed.");
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

irq::request_result kb_interrupt(const void *regs, u64 extra_data, u64 user_data)
{
    kb_buffer_t *buffer = (kb_buffer_t *)user_data;
    if (unlikely(buffer == nullptr))
        return irq::request_result::no_handled;

    u8 data;
    data = io_in8(data_port);
    kb_data_t kdata;
    kdata.set(timer::get_high_resolution_time(), data);
    buffer->write_with() = kdata;

    return irq::request_result::ok;
}

bool set_led(u8 s)
{
    wait_for_write();
    io_out8(data_port, 0xED);

    wait_for_read();
    if (io_in8(data_port) != 0xFA)
    {
        trace::warning("Can't set led (no ACK)");
        return false;
    }

    wait_for_write();
    io_out8(data_port, s);

    wait_for_read();
    if (io_in8(data_port) != 0xFA)
    {
        trace::warning("Can't set led (no ACK)");
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
        trace::warning("Unable to get keyboard id.");
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
    kb_buffer_t &buffer = ((kb_device *)dev)->buffer;
    uctx::UnInterruptableContext icu;

    io_out8(cmd_port, 0x20);
    wait_for_read();
    u8 old_status = io_in8(data_port);
    old_status &= ~0b00010000;
    old_status |= 0b01000001;

    auto id = get_keyboard_id();
    trace::debug("keyboard id = ", (void *)(u64)id);

    ((kb_device *)dev)->id = id;

    blink_led();

    // reset led
    set_led(0);

    ((kb_device *)dev)->led_status = 0;

    io_out8(cmd_port, 0x60);

    wait_for_write();
    io_out8(data_port, old_status);

    flush();

    APIC::io_entry entry;
    entry.dest_apic_id = APIC::local_ID();
    entry.is_level_trigger_mode = false;
    entry.is_logic_mode = false;
    entry.is_disable = true;
    entry.low_level_polarity = false;
    entry.delivery_mode = APIC::io_entry::mode_t::fixed;

    auto intr = APIC::io_irq_setup(0x1, &entry);
    irq::insert_request_func(intr, kb_interrupt, (u64)&buffer);
    APIC::io_enable(0x1);
    return true;
}

void kb_driver::cleanup(::dev::device *dev)
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (kb_buffer_t *)(dev->get_user_data()));
}

bool get_key(io::keyboard_request_t *req, kb_device *dev, u8 *key, u64 *timestamp, bool *release)
{
    kb_data_t kb_data;
    static u8 last_prefix_count = 0;
    static u8 last_prefix[2];

    while (dev->buffer.read(&kb_data))
    {
        u8 k = kb_data.get_key();
        if (last_prefix_count == 0)
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
                last_prefix[last_prefix_count++] = k;
            }
            else
            {
                trace::warning("Unknow scan code. ", (void *)(u64)k);
            }
        }
        else if (last_prefix_count == 1)
        {
            if (last_prefix[0] == 0xE0)
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
                last_prefix_count = 0;
                return true;
            }
            else if (last_prefix[0] == 0xE1)
            {
                last_prefix[last_prefix_count++] = k;
            }
            else
            {
                trace::warning("Unknow scan code.", (void *)(u64)last_prefix[0], ",", (void *)(u64)k);
            }
        }
        else if (last_prefix_count == 2)
        {
            if ((k == 0x45 && last_prefix[1] == 0x1D) || (k == 0xC5 && last_prefix[1] == 0x9D))
            {
                *key = (u8)input::key::pause;
                *release = false;
                *timestamp = kb_data.get_timestamp(timer::get_high_resolution_time());
                last_prefix_count = 0;
                return true;
            }
            else
            {
                trace::warning("Unknow scan code.", (void *)(u64)last_prefix[0], ",", (void *)(u64)last_prefix[1], ",",
                               (void *)(u64)k);
                last_prefix_count = 0;
            }
        }
        else
        {
            trace::warning("Unknow scan code. Unknow prefix. ", last_prefix_count);
        }
    }
    return false;
}

void kb_driver::on_io_request(io::request_t *request)
{
    kassert(request->type == io::chain_number::keyboard, "Error driver state.");
    io::keyboard_request_t *req = (io::keyboard_request_t *)request;
    io::keyboard_result_t &ret = req->result;
    kb_device *dev = (kb_device *)req->get_current_device();
    switch (req->cmd_type)
    {
        case io::keyboard_request_t::command::get_key: {
            if (get_key(req, dev, &ret.cmd_type.get.key, &ret.cmd_type.get.timestamp, &ret.cmd_type.get.release))
            {
                ret.poll_status = 0;
            }
            else
            {
                ret.poll_status = 1;
            }
            break;
        }
        case io::keyboard_request_t::command::set_repeat: {

            break;
        }
        case io::keyboard_request_t::command::set_led_cas: {
            uctx::UnInterruptableContext icu;

            if (req->cmd_param)
                dev->led_status |= 0b100;
            else
                dev->led_status &= ~(0b100);
            set_led(dev->led_status);
            ret.cmd_type.set.ok = true;
            break;
        }
        case io::keyboard_request_t::command::set_led_scroll: {
            uctx::UnInterruptableContext icu;
            if (req->cmd_param)
                dev->led_status |= 0b001;
            else
                dev->led_status &= ~(0b001);
            set_led(dev->led_status);
            ret.cmd_type.set.ok = true;
            break;
        }
        case io::keyboard_request_t::command::set_led_num: {
            uctx::UnInterruptableContext icu;
            if (req->cmd_param)
                dev->led_status |= 0b010;
            else
                dev->led_status &= ~(0b010);
            set_led(dev->led_status);
            ret.cmd_type.set.ok = true;
            break;
        }
    }
}

//===============================

irq::request_result mouse_interrupt(const void *regs, u64 extra_data, u64 user_data)
{
    mouse_buffer_t *buffer = (mouse_buffer_t *)user_data;
    if (unlikely(buffer == nullptr))
        return irq::request_result::no_handled;

    auto data = io_in8(data_port);
    mouse_data_t md;
    md.set(timer::get_high_resolution_time(), data);
    buffer->write_with() = md;
    return irq::request_result::ok;
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
        trace::warning("Can't set mouse rate (no ACK)");
        return false;
    }
    wait_for_write();
    io_out8(cmd_port, 0xD4);
    wait_for_write();
    io_out8(data_port, rate);
    wait_for_read();
    if (io_in8(data_port) != 0xFA)
    {
        trace::warning("Can't set mouse rate (no ACK)");
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
        trace::warning("Can't get mouse id.");
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
    mouse_buffer_t &buffer = ((mouse_device *)dev)->buffer;
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
    ((mouse_device *)dev)->id = id;
    trace::debug("mouse id = ", id);

    wait_for_write();
    io_out8(cmd_port, 0x60);
    wait_for_write();
    io_out8(data_port, old_status);

    APIC::io_entry entry;
    entry.dest_apic_id = APIC::local_ID();
    entry.is_level_trigger_mode = false;
    entry.is_logic_mode = false;
    entry.is_disable = true;
    entry.low_level_polarity = false;
    entry.delivery_mode = APIC::io_entry::mode_t::fixed;

    auto intr = APIC::io_irq_setup(12, &entry);
    irq::insert_request_func(intr, mouse_interrupt, (u64)&buffer);

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

bool mouse_get(mouse_buffer_t &buffer, io::mouse_data *data, bool type3)
{
    static u8 last_index = 0;
    static u8 last_data[4];
    mouse_data_t dt;
    while (buffer.read(&dt))
    {
        switch (last_index)
        {
            case 0:
                last_data[last_index++] = dt.data;
                if (!(dt.data & 0b1000))
                {
                    trace::warning("Unkown mouse data.");
                    last_index = 0;
                }
                break;
            case 1:
                last_data[last_index++] = dt.data;
                break;
            case 2:
                last_data[last_index++] = dt.data;
                if (type3)
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
                data->movement_z = 8 - (8 - last_data[3] & 0xF);

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
    kassert(request->type == io::chain_number::mouse, "Error driver state.");
    io::mouse_request_t *req = (io::mouse_request_t *)request;
    io::mouse_result_t &ret = req->result;
    mouse_device *dev = (mouse_device *)req->get_current_device();

    mouse_buffer_t &buffer = dev->buffer;
    switch (req->cmd_type)
    {
        case io::mouse_request_t::command::get: {
            if (mouse_get(buffer, &ret.cmd_type.get, dev->id == 0))
            {
                ret.poll_status = 0;
            }
            else
            {
                ret.poll_status = 1;
            }
        }
        break;
        case io::mouse_request_t::command::set_speed: {
            /// TODO: set speed for mouse
            ret.cmd_type.set.ok = true;
            break;
        }
        default:
            break;
    }
}

} // namespace arch::device::chip8042
