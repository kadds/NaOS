#include "kernel/arch/dev/serial/8042.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/arch/acpi/acpi.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/io_apic.hpp"
#include "kernel/arch/local_apic.hpp"
#include "kernel/dev/device.hpp"
#include "kernel/input/key.hpp"
#include "kernel/io/io_manager.hpp"
#include "kernel/irq.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/timer.hpp"
#include "kernel/trace.hpp"
#include "kernel/types.hpp"

namespace arch::device::chip8042
{

constexpr io_port data_port = 0x60;
constexpr io_port cmd_port = 0x64;
constexpr io_port cmd_status_port = 0x64;


enum class chip8042_device_clazz {
    unknown,
    mouse,
    keyboard,
};

struct chip8042_scan_device_info_t {
    chip8042_device_clazz clazz = chip8042_device_clazz::unknown;
    u32 id = 0;
};

struct chip8042_scan_info_t {
    chip8042_scan_device_info_t devices[2];
};

chip8042_scan_info_t system_chip8042;
chip8042_scan_info_t try_scan_8042();

void init()
{
    system_chip8042 = try_scan_8042();
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
    volatile int val = 0;

    while (target > timer::get_high_resolution_time())
    {
        for (int i = 0; i < 1000; i++)
        {
            val = val * i;
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
        trace::debug("8042 flush: read ", trace::hex((u64)d));
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

bool wait_for_read_us(u32 us)
{
    u64 target = us + timer::get_high_resolution_time();
    volatile int val = 0;

    while (target > timer::get_high_resolution_time() && !((io_in8(cmd_status_port)) & 0x1))
    {
        for (int i = 0; i < 1000; i++)
        {
            val = val * i;
        }
    }
    return io_in8(cmd_status_port) & 0x1;
}

void write_controller(u8 command_byte) {
    wait_for_write();
    io_out8(cmd_port, command_byte);
    delay();
}

void write_controller_data(u8 data_byte) {
    wait_for_write();
    io_out8(data_port, data_byte);
    delay();
}

u8 read_controller() {
    wait_for_read();
    return io_in8(data_port);
}

void write_first_ps2(u8 data_byte) {
    wait_for_write();
    io_out8(data_port, data_byte);
    delay();
}

void write_second_ps2(u8 data_byte) {
    wait_for_write();
    io_out8(cmd_port, 0xD4);
    wait_for_write();
    io_out8(data_port, data_byte);
    delay();
}

void write_ps2(int index, u8 data_byte) {
    if (index == 0) {
        write_first_ps2(data_byte);
    } else {
        write_second_ps2(data_byte);
    }
}

u8 read_ps2() {
    wait_for_read();
    return io_in8(data_port);
}

bool ack() {
    wait_for_read();
    return io_in8(data_port) == 0xFA;
}

u8 read_ps2_fast() {
    return io_in8(data_port);
}

u8 read_ps2_optional() {
    wait_for_read_us(100);
    return io_in8(data_port);
}

bool self_test_8042() {
    write_controller(0xAA);

    if (read_controller() != 0x55)
    {
        trace::debug("8042 self check failed");
        return false;
    }
    return true;
}

u16 get_device_id(int index)
{
    // disable scan
    write_ps2(index, 0xF5);

    if (!ack()) // ACK
    {
        trace::warning("8042 disable scan fail");
        return 0xFFFF;
    }

    // query
    write_ps2(index, 0xF2);

    if (!ack()) // ACK
    {
        trace::warning("get device id failed");
        return 0xFFFF;
    }

    u16 id = read_ps2();
    u16 id2 = 0;
    if (wait_for_read_us(100)) 
    {
        id2 = read_ps2();
        // enable scan
        write_ps2(index, 0xF4);
        read_ps2_optional();
        return (id << 8) | id2;
    } 
    else 
    {
        // enable scan
        write_ps2(index, 0xF4);
        read_ps2_optional();
        return id;
    }
}

bool test_device_8042(int index) {
    u8 device = index == 0 ? 0xAB : 0xA9;

    write_controller(device);
    if (read_controller() != 0x00)
    {
        trace::debug("8042 device check failed");
        return false;
    }
    return true;
}

bool config_8042(u8 clear_flags, u8 set_flags) {
    // config 8042
    write_controller(0x20);
    u8 old_status = read_controller();
    bool has_dual_channel = old_status & 0b10000;
    old_status &= ~clear_flags;
    old_status |= set_flags;

    write_controller(0x60);
    write_controller_data(old_status);
    return has_dual_channel;
}

void fill_device_type(chip8042_scan_device_info_t &dev) {
    if (dev.id != 0xFF) {
        if ((dev.id >> 8) == 0xAB) {
            dev.clazz = chip8042_device_clazz::keyboard;
            trace::debug("keyboard id ", trace::hex(dev.id));
        } else {
            dev.clazz = chip8042_device_clazz::mouse;
            trace::debug("mouse id ", trace::hex(dev.id));
        }
    }
}

chip8042_scan_info_t try_scan_8042()
{
    // disable translation
    chip8042_scan_info_t info;
    if (!ACPI::is_8042_device_exists()) {
        trace::warning("8042 not exists");
        return info;
    }

    // disable device
    write_controller(0xAD);
    write_controller(0xA7);

    // disable irq
    bool has_dual_channel = config_8042(0b01000011, 0);

    flush();

    if (!self_test_8042()) {
        return info;
    }
    // 2 channel test
    if (has_dual_channel) {
        // TODO
    }
    // frist device
    if (test_device_8042(0xAB)) {
        // read id
        info.devices[0].id = get_device_id(0);
        fill_device_type(info.devices[0]);
    }
    // second device
    if (test_device_8042(0xA9)) {
        info.devices[1].id = get_device_id(1);
        fill_device_type(info.devices[1]);
    }
    // enable device
    write_controller(0xAE);
    write_controller(0xA8);

    flush();

    // enable irq
    config_8042(0, 0b11);

    return info;
}

freelibcxx::vector<::dev::device *> kb_device_class::try_scan()
{
    freelibcxx::vector<::dev::device *> devs(memory::KernelCommonAllocatorV);
    for (int i = 0; i < 2; i++) 
    {
        if (system_chip8042.devices[i].clazz == chip8042_device_clazz::keyboard) 
        {
            devs.push_back(memory::New<kb_device>(memory::KernelCommonAllocatorV, i));
        }
    }
    if (devs.empty()) 
    {
        trace::warning("no keyboard found");
    }
    return devs;
}

freelibcxx::vector<::dev::device *> mouse_device_class::try_scan()
{
    freelibcxx::vector<::dev::device *> devs(memory::KernelCommonAllocatorV);
    for (int i = 0; i < 2; i++) 
    {
        if (system_chip8042.devices[i].clazz == chip8042_device_clazz::mouse) 
        {
            devs.push_back(memory::New<mouse_device>(memory::KernelCommonAllocatorV, i));
        }
    }
    if (devs.empty()) 
    {
        trace::warning("no mouse found");
    }
    return devs;
}

bool get_key(kb_device *dev, io::keyboard_data *data);

irq::request_result kb_interrupt(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    kb_device *dev = (kb_device *)user_data;

    if (unlikely(dev == nullptr))
        return irq::request_result::no_handled;

    u8 data = read_ps2_fast();
    kb_data_t kb_data;
    kb_data.set(timer::get_high_resolution_time(), data);
    dev->buffer.write(kb_data);

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

bool set_led(int index, u8 s)
{
    write_ps2(index, 0xED);
    if (!ack()) 
    {
        trace::warning("set led (no ACK) failed");
        return false;
    }

    write_ps2(index, s);
    return true;
}

void blink_led(int index)
{
    set_led(index, 0b111);
    delay();
    set_led(index, 0b000);
    delay();
    set_led(index, 0b000);
    delay();
    set_led(index, 0b111);
}


bool kb_driver::setup(::dev::device *dev)
{
    kb_device *kb_dev = ((kb_device *)dev);
    uctx::UninterruptibleContext icu;

    blink_led(kb_dev->port_index);
    kb_dev->led_status = 0;

    // select scan code set 1
    write_ps2(kb_dev->port_index, 0xF0);
    write_ps2(kb_dev->port_index, 0x1);
    if (!ack()) 
    {
        trace::warning("set keyboard codeset fail");
    }

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
                trace::warning("Unknow scan code ", trace::hex((u64)k));
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
                trace::warning("Unknow scan code ", trace::hex((u64)dev->last_prefix[0]), ",", trace::hex((u64)k));
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
                trace::warning("Unknow scan code ", trace::hex((u64)dev->last_prefix[0]), ",",
                               trace::hex((u64)dev->last_prefix[1]), ",", trace::hex((u64)k));
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
                    dev->io_list.push_back(req);
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
            set_led(dev->port_index, dev->led_status);
            status.failed_code = 0;
            break;
        }
        case io::keyboard_request_t::command::set_led_scroll: {
            uctx::UninterruptibleContext icu;
            if (req->cmd_param)
                dev->led_status |= 0b001;
            else
                dev->led_status &= ~(0b001);
            set_led(dev->port_index, dev->led_status);
            status.failed_code = 0;
            break;
        }
        case io::keyboard_request_t::command::set_led_num: {
            uctx::UninterruptibleContext icu;
            if (req->cmd_param)
                dev->led_status |= 0b010;
            else
                dev->led_status &= ~(0b010);
            set_led(dev->port_index, dev->led_status);
            status.failed_code = 0;
            break;
        }
    }
}

//===============================
bool mouse_get(mouse_device *dev, io::mouse_data *data);

irq::request_result mouse_interrupt(const irq::interrupt_info *inter, u64 extra_data, u64 user_data)
{
    auto dev = (mouse_device *)user_data;
    if (unlikely(dev == nullptr))
        return irq::request_result::no_handled;

    auto data = read_ps2_fast();
    mouse_data_t md;
    md.set(timer::get_high_resolution_time(), data);

    dev->buffer.write(md);
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

bool set_mouse_rate(int port_index, u8 rate)
{
    write_ps2(port_index, 0xF3);
    if (!ack())
    {
        trace::warning("set mouse rate (no ACK) failed");
        return false;
    }
    write_ps2(port_index, rate);
    if (!ack())
    {
        trace::warning("set mouse rate (no ACK) failed");
        return false;
    }
    return true;
}

bool mouse_driver::setup(::dev::device *dev)
{
    mouse_device *ms_dev = (mouse_device *)dev;

    // try to enable z-extensions
    auto &info = system_chip8042.devices[ms_dev->port_index];

    if (info.id == 0)
    {
        set_mouse_rate(ms_dev->port_index, 200);
        set_mouse_rate(ms_dev->port_index, 100);
        set_mouse_rate(ms_dev->port_index, 80);
        delay();
        u16 id = get_device_id(ms_dev->port_index);
        if (id == 0x3) 
        {
            info.id = id;
            trace::debug("mouse extension id ", trace::hex(id));
            ms_dev->extension_z = true;
            // try to enable 5 buttons
            set_mouse_rate(ms_dev->port_index, 200);
            set_mouse_rate(ms_dev->port_index, 200);
            set_mouse_rate(ms_dev->port_index, 80);
            delay();
            id = get_device_id(ms_dev->port_index);
            if (id == 0x4) 
            {
                info.id = id;
                trace::debug("mouse extension id ", trace::hex(id));
                ms_dev->extension_buttons = true;
            }
        }
    }
    set_mouse_rate(ms_dev->port_index, 100);

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

    // enable it
    write_ps2(ms_dev->port_index, 0xF4);
    if (!ack()) 
    {
        trace::warning("enable mouse report fail(no ACK)");
    }

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
                    trace::warning("Unknown mouse data. ", trace::hex(dt.data));
                    last_index = 0;
                }
                break;
            case 1:
                last_data[last_index++] = dt.data;
                break;
            case 2:
                last_data[last_index++] = dt.data;
                if (!dev->extension_z)
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
                if (!dev->extension_buttons) 
                {
                    data->movement_z = (u16) last_data[3];
                } 
                else
                {
                    data->movement_z = 8 - ((8 - last_data[3]) & 0xF);
                }

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
                    dev->io_list.push_back(req);
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
