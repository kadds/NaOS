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

void wait_for_write()
{
    while (io_in8(cmd_status_port) & 0x2)
    {
    }
}

void wait_for_read()
{
    while (io_in8(cmd_status_port) & 0x1)
    {
    }
}

void wait_for_mouse_read()
{
    while (io_in8(cmd_status_port) & 0b100000)
    {
    }
}

void delay()
{
    u64 us = timer::get_high_resolution_time() + 100;
    while (timer::get_high_resolution_time() < us)
    {
    }
}

::dev::device *kb_device_class::try_scan(int index)
{
    if (index == 0)
    {
        // self check failed
        if (!(io_in8(cmd_status_port) & 0x4))
        {
            return nullptr;
        }
        io_out8(cmd_port, 0xAA);
        delay();
        if (io_in8(data_port) != 0x55)
        {
            return nullptr;
        }
        io_out8(cmd_port, 0xAB);
        delay();
        if (io_in8(data_port) != 0x00)
        {
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
        // self check failed
        if (!(io_in8(cmd_status_port) & 0x4))
        {
            return nullptr;
        }
        io_out8(cmd_port, 0xAA);
        delay();

        if (io_in8(data_port) != 0x55)
        {
            return nullptr;
        }
        io_out8(cmd_port, 0xA9);
        delay();

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

    if ((io_in8(cmd_status_port) & 0x1))
    {
        u8 data;
        data = io_in8(data_port);
        kb_data_t kdata;
        kdata.set(timer::get_high_resolution_time(), data);
        buffer->write_with() = kdata;
    }

    return irq::request_result::ok;
}

bool set_led(u8 s)
{
    wait_for_write();
    io_out8(cmd_port, 0x60);
    wait_for_write();
    io_out8(data_port, 0xED);

    delay();
    if (io_in8(data_port) == 0xFA)
    {
        // ACK
        wait_for_write();
        io_out8(data_port, s);
        delay();
        if (io_in8(data_port) == 0xFA)
        {
            io_out8(data_port, 0b1000000);
            return true;
        }
    }
    return false;
}

bool kb_driver::setup(::dev::device *dev)
{
    kb_buffer_t &buffer = ((kb_device *)dev)->buffer;
    uctx::UnInterruptableContext icu;
    wait_for_write();
    io_out8(cmd_port, 0x20);
    delay();
    u8 old_status = io_in8(data_port);
    old_status &= ~0b00010000;
    old_status |= 0b01000001;

    wait_for_write();
    io_out8(cmd_port, 0xAE);
    wait_for_write();

    io_out8(data_port, 0xF2);

    delay();

    auto f1 = io_in8(data_port);
    if (f1 != 0xFA) // ACK
    {
        trace::warning("Unable to get keyboard id.");
    }
    delay();

    auto id = io_in8(data_port); // 0xAB
    trace::debug("keyboard id = ", (void *)(u64)id);
    delay();
    io_in8(data_port); // 0x83

    ((kb_device *)dev)->id = id;

    // reset led
    set_led(0);

    ((kb_device *)dev)->led_status = 0;

    wait_for_write();
    io_out8(cmd_port, 0x60);

    wait_for_write();
    io_out8(data_port, old_status);

    // clean output
    wait_for_write();
    io_out8(cmd_port, 0xAE);
    wait_for_write();
    io_out8(data_port, 0xF4);
    delay();
    io_in8(data_port); // ACK

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
            wait_for_write();
            io_out8(cmd_port, 0x60);
            wait_for_write();
            io_out8(data_port, 0xED);
            wait_for_write();
            if (req->cmd_param)
                dev->led_status |= 0b100;
            else
                dev->led_status &= ~(0b100);
            io_out8(data_port, dev->led_status);
            ret.cmd_type.set.ok = true;
            break;
        }
        case io::keyboard_request_t::command::set_led_scroll: {
            uctx::UnInterruptableContext icu;

            wait_for_write();
            io_out8(cmd_port, 0x60);
            wait_for_write();
            io_out8(data_port, 0xED);
            wait_for_write();
            if (req->cmd_param)
                dev->led_status |= 0b001;
            else
                dev->led_status &= ~(0b001);
            io_out8(data_port, dev->led_status);
            ret.cmd_type.set.ok = true;
            break;
        }
        case io::keyboard_request_t::command::set_led_num: {
            uctx::UnInterruptableContext icu;

            wait_for_write();
            io_out8(cmd_port, 0x60);
            wait_for_write();
            io_out8(data_port, 0xED);
            wait_for_write();
            if (req->cmd_param)
                dev->led_status |= 0b010;
            else
                dev->led_status &= ~(0b010);
            io_out8(data_port, dev->led_status);
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
    buffer->write_with() = data;
    return irq::request_result::ok;
}

void mouse_get(u64 vector, u64 user_data)
{
    mouse_buffer_t *buffer = (mouse_buffer_t *)user_data;
    u8 data;
    static u8 last_index = 0;
    static u8 last_package[4];
    while (buffer->read(&data))
    {
        switch (last_index)
        {
            case 0:
                last_index++;
                break;

            default:
                break;
        }
        timer::get_high_resolution_time();
    }

    // if (io_in8(cmd_status_port) & 0b100000)
    // {
    //     data[3] = io_in8(data_port);
    // }

    // mouse_data_t mdata;
    // mdata.set(timer::get_high_resolution_time(), data[1], data[2], data[3]);

    // if (data[0] & (0b10000))
    // {
    //     x = -x;
    // }
    // if (data[1] & (0b100000))
    // {
    //     y = -y;
    // }

    // trace::debug("mouse to (", x, ",", y, ")");
}

bool mouse_driver::setup(::dev::device *dev)
{
    mouse_buffer_t &buffer = ((mouse_device *)dev)->buffer;

    wait_for_write();
    io_out8(cmd_port, 0xA8);

    wait_for_write();
    io_out8(cmd_port, 0x20);
    delay();
    u8 old_status = io_in8(data_port);
    old_status &= ~0b00100000;
    old_status |= 0b00000010;

    wait_for_write();
    io_out8(cmd_port, 0xD4);
    wait_for_write();
    io_out8(data_port, 0xF2);

    delay();
    auto id = io_in8(data_port);
    if (id != 0xFA)
    {
        trace::warning("Can't get mouse id.");
    }
    else
    {
        delay();
        id = io_in8(data_port);
        trace::debug("mouse id = ", id);
        ((mouse_device *)dev)->id = id;
    }

    wait_for_write();
    io_out8(cmd_port, 0x60);
    wait_for_write();
    io_out8(data_port, old_status);

    delay();

    APIC::io_entry entry;
    entry.dest_apic_id = APIC::local_ID();
    entry.is_level_trigger_mode = false;
    entry.is_logic_mode = false;
    entry.is_disable = true;
    entry.low_level_polarity = false;
    entry.delivery_mode = APIC::io_entry::mode_t::fixed;

    auto intr = APIC::io_irq_setup(12, &entry);
    irq::insert_request_func(intr, mouse_interrupt, (u64)&buffer);

    APIC::io_enable(12);

    wait_for_write();
    io_out8(cmd_port, 0xD4);

    wait_for_write();
    io_out8(data_port, 0xF4);

    return true;
}

void mouse_driver::cleanup(::dev::device *dev)
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (mouse_buffer_t *)(dev->get_user_data()));
}

void mouse_driver::on_io_request(io::request_t *request) {}

} // namespace arch::device::chip8042
