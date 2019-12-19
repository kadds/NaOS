#include "kernel/arch/dev/serial/8042.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/irq.hpp"
#include "kernel/timer.hpp"
#include "kernel/types.hpp"
namespace arch::device::chip8042
{

io_port data_port = 0x60;
io_port cmd_port = 0x64;
io_port cmd_status_port = 0x64;

io_port kb_data_port = 0x64;
io_port kb_control_port = 0x60;

io_port mouse_control_port = 0x60;
io_port mouse_data_port = 0x64;

void init()
{
    kb_device_class dc;
    if (dev::enum_device(&dc))
    {
        trace::debug("Find 8042 keyboard device. Loading driver.");
        auto dev = memory::New<kb_driver>(memory::KernelCommonAllocatorV);
        if (dev::add_driver(dev) == ::dev::null_num)
        {
            trace::warning("Load 8042 keyboard driver failed.");
            memory::Delete<>(memory::KernelCommonAllocatorV, dev);
        }
    }

    mouse_device_class ds;
    if (dev::enum_device(&ds) > 0)
    {
        trace::debug("Find 8042 mouse device. Loading driver.");
        auto dev = memory::New<mouse_driver>(memory::KernelCommonAllocatorV);
        if (dev::add_driver(dev) == ::dev::null_num)
        {
            trace::warning("Load 8042 mouse driver failed.");
            memory::Delete<>(memory::KernelCommonAllocatorV, dev);
        }
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
        if (io_in8(data_port) == 0x55)
        {
            return memory::New<kb_device>(memory::KernelCommonAllocatorV);
        }
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

        if (io_in8(data_port) == 0x55)
        {
            return memory::New<kb_device>(memory::KernelCommonAllocatorV);
        }
    }
    return nullptr;
}

irq::request_result kb_interrupt(const void *regs, u64 extra_data, u64 user_data)
{
    kb_buffer_t *buffer = (kb_buffer_t *)user_data;
    if (unlikely(buffer == nullptr))
        return irq::request_result::no_handled;
    while ((io_in8(cmd_status_port) & 0x1))
    {
        u8 data;
        data = io_in8(data_port);
        kb_data_t kdata;
        trace::debug("input ", (char)data);
        kdata.set(timer::get_high_resolution_time(), data);
        buffer->write_with() = kdata;
    }

    return irq::request_result::ok;
}

bool kb_driver::setup(::dev::device *dev)
{
    kb_buffer_t &buffer = ((kb_device *)dev)->buffer;
    APIC::io_entry entry;
    entry.dest_apic_id = APIC::local_ID();
    entry.is_level_trigger_mode = false;
    entry.is_logic_mode = false;
    entry.is_disable = true;
    entry.low_level_polarity = false;
    entry.delivery_mode = APIC::io_entry::mode_t::fixed;

    auto intr = APIC::io_irq_setup(0x1, &entry);
    irq::insert_request_func(intr, kb_interrupt, (u64)&buffer);

    wait_for_write();
    // write command
    io_out8(cmd_port, 0x20);

    u8 old_status = io_in8(data_port);
    old_status &= ~0b00010000;
    old_status |= 0b01000001;

    wait_for_write();
    // write command
    io_out8(cmd_port, 0x60);

    wait_for_write();
    // irq1
    // send init data
    io_out8(data_port, old_status);

    APIC::io_enable(0x1);
    return true;
}

void kb_driver::cleanup(::dev::device *dev)
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (kb_buffer_t *)(dev->get_user_data()));
}

u64 kb_driver::get_key_data(::dev::device *dev, kb_data_t *data, u64 max_len) {}

void kb_driver::on_io_request(io::request_t *request) {}

irq::request_result mouse_interrupt(const void *regs, u64 extra_data, u64 user_data)
{
    mouse_buffer_t *buffer = (mouse_buffer_t *)user_data;
    if (unlikely(buffer == nullptr))
        return irq::request_result::no_handled;
    u8 data;
    data = io_in8(data_port);
    mouse_data_t mdata;
    trace::debug("mouse move ");

    // timer::get_high_resolution_time();
    // buffer->write_with() = data;
    return irq::request_result::ok;
}

bool mouse_driver::setup(::dev::device *dev)
{
    mouse_buffer_t &buffer = ((mouse_device *)dev)->buffer;
    APIC::io_entry entry;
    entry.dest_apic_id = APIC::local_ID();
    entry.is_level_trigger_mode = false;
    entry.is_logic_mode = false;
    entry.is_disable = true;
    entry.low_level_polarity = false;
    entry.delivery_mode = APIC::io_entry::mode_t::fixed;

    auto intr = APIC::io_irq_setup(0x11, &entry);
    irq::insert_request_func(intr, mouse_interrupt, (u64)&buffer);

    wait_for_write();
    // write command
    io_out8(cmd_port, 0x20);

    u8 old_status = io_in8(data_port);
    old_status &= ~0b00100000;
    old_status |= 0b00000010;

    wait_for_write();
    // write command
    io_out8(kb_control_port, 0x60);

    wait_for_write();
    // irq1
    // send init data
    io_out8(kb_data_port, old_status);
    APIC::io_enable(0x11);

    return true;
}

void mouse_driver::cleanup(::dev::device *dev)
{
    memory::Delete<>(memory::KernelCommonAllocatorV, (kb_buffer_t *)(dev->get_user_data()));
}
void mouse_driver::on_io_request(io::request_t *request) {}
u64 mouse_driver::get_mouse_data(::dev::device *dev, kb_data_t *data, u64 max_len) { return 0; }
} // namespace arch::device::chip8042
