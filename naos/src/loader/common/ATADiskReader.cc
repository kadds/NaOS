#include "loader/ATADiskReader.hpp"
#include "loader/loader.hpp"

short port[] = {0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6,
                0x170, 0x171, 0x172, 0x173, 0x174, 0x175, 0x176, 0x177, 0x376};

void ATADiskReader::read_data(void *dest, u64 byte_count)
{
    unsigned short *dt = (unsigned short *)dest;
    if (byte_count == 0)
        return;
    bytes_read += byte_count;
    u64 i = 0;
    if (dt == nullptr)
    {

        for (; i < byte_count; i += 2)
            io_in16(get_port(ata_port_type::data));
        if (i > byte_count)
            io_in8(get_port(ata_port_type::data));
    }
    else
    {
        for (; i < byte_count; i += 2)
        {
            u16 data = io_in16(get_port(ata_port_type::data));
            *dt++ = data;
        }
        if (i > byte_count)
            *((u8 *)dt) = io_in8(get_port(ata_port_type::data));
    }
}
bool ATADiskReader::has_data()
{
    u8 v = io_in8(get_port(ata_port_type::status_command));
    if ((v & 0x80) != 0)
        return false;
    if ((v & 0x21) != 0)
        return false; // error
    if ((v & 0x8) != 0)
        return true; // error
    return false;
}
short ATADiskReader::get_port(ata_port_type port_type)
{
    return port[sizeof(port) / sizeof(port[0]) * (int)disk + (int)port_type];
}
static short get_port(ata_disk disk, ata_port_type port_type)
{
    return port[sizeof(port) / sizeof(port[0]) * (int)disk + (int)port_type];
}
int ATADiskReader::disk_wait()
{
    while (1)
    {
        u8 v = io_in8(get_port(ata_port_type::status_command));
        if ((v & 0x80) != 0)
            continue;
        if ((v & 0x21) != 0)
            return -1;          // error
        if ((v & 0xC0) == 0x40) // ready
        {
            return v;
        }
    }
}

int ATADiskReader::drq_wait()
{
    while (1)
    {
        u8 v = io_in8(get_port(ata_port_type::status_command));
        if ((v & 0x80) != 0)
            continue;
        if ((v & 0x21) != 0)
            return -1;         // error
        if ((v & 0x88) == 0x8) // read data
        {
            return v;
        }
    }
}
void ATADiskReader::sleep_wait()
{
    io_in8(get_port(ata_port_type::status_command));
    io_in8(get_port(ata_port_type::status_command));
    io_in8(get_port(ata_port_type::status_command));
    io_in8(get_port(ata_port_type::status_command));
}

void ATADiskReader::out_lba48(u64 lba, u16 lba_count)
{
    u8 low, high;
    low = lba_count;
    high = lba_count >> 8;
    u8 lba1 = (u8)lba;
    u8 lba2 = (u8)(lba >> 8);
    u8 lba3 = (u8)(lba >> 16);
    u8 lba4 = (u8)(lba >> 24);
    u8 lba5 = (u8)(lba >> 32);
    u8 lba6 = (u8)(lba >> 40);
    if (master)
        io_out8(get_port(ata_port_type::drive_read), 0xE0);
    else
        io_out8(get_port(ata_port_type::drive_read), 0xF0);
    io_out8(get_port(ata_port_type::selector_count), (u8)high); // high
    io_out8(get_port(ata_port_type::selector_number), lba4);
    io_out8(get_port(ata_port_type::cylinder_low), lba5);
    io_out8(get_port(ata_port_type::cylinder_high), lba6);

    io_out8(get_port(ata_port_type::selector_count), (u8)low);
    io_out8(get_port(ata_port_type::selector_number), lba1);
    io_out8(get_port(ata_port_type::cylinder_low), lba2);
    io_out8(get_port(ata_port_type::cylinder_high), lba3);

    io_out8(get_port(ata_port_type::status_command), 0x24);
}

ATADiskReader::ATADiskReader(ata_disk disk, bool master) : disk(disk), master(master), bytes_read(0) {}
int ATADiskReader::reset()
{
    io_out8(get_port(ata_port_type::control), 0x6);
    io_out8(get_port(ata_port_type::control), 0);
    sleep_wait();
    io_in8(get_port(ata_port_type::status_command));
    if (disk_wait() == -1)
        return -1;
    return 0;
}
int ATADiskReader::error_code() { return io_in8(get_port(ata_port_type::error_feature)); }

int ATADiskReader::read_data_lba48(void *dest, u64 start_lba, u32 offset_bytes, u32 size_of_bytes)
{
    constexpr u32 bytes_per_selector = 512;
    constexpr u32 max_selector_per_read = 1 << 16u;
    // add start_lba if offset > bytes_per_selector
    start_lba += offset_bytes / bytes_per_selector;
    offset_bytes = offset_bytes % bytes_per_selector;

    // how many LBA should be read
    u64 lba_count = (offset_bytes + size_of_bytes + bytes_per_selector - 1) / bytes_per_selector;

    u64 times = (lba_count + max_selector_per_read - 1) / (max_selector_per_read);
    u64 rest_lba = lba_count % max_selector_per_read;

    u8 *dst = (u8 *)dest;

    u64 current_selector_count;
    for (u64 i = 0; i < times; i++)
    {
        if (disk_wait() == -1)
            return -1;
        if (times == i + 1)
            // last time
            current_selector_count = rest_lba;
        else
            current_selector_count = max_selector_per_read;
        out_lba48(start_lba, current_selector_count);
        if (i == 0)
        {
            if (drq_wait() == -1)
                return -1;
            // first time, read offset
            read_data(nullptr, offset_bytes);
            if (size_of_bytes + offset_bytes <= bytes_per_selector)
            {
                // last
                read_data(dst, size_of_bytes);
                read_data(nullptr, bytes_per_selector - offset_bytes - size_of_bytes);
                sleep_wait();
                if (has_data())
                    return -1;
                return 0;
            }
            else
            {
                read_data(dst, bytes_per_selector - offset_bytes);
                dst += bytes_per_selector - offset_bytes;
                size_of_bytes -= bytes_per_selector;
                sleep_wait();
            }
        }

        for (u64 selector = 1; selector < current_selector_count - 1; selector++)
        {
            if (drq_wait() == -1)
                return -1;

            read_data(dst, bytes_per_selector);
            dst += bytes_per_selector;
            size_of_bytes -= bytes_per_selector;
            sleep_wait();
        }

        if (times == i + 1)
        {
            if (drq_wait() == -1)
                return -1;

            // last
            read_data(dst, size_of_bytes);
            read_data(nullptr, bytes_per_selector - size_of_bytes);
            sleep_wait();
            if (has_data())
                return -1;
            return 0;
        }
    }

    return 0;
}

ATADiskReader *ATADiskReader::get_available_disk()
{
    short pread[] = {0xA0, 0xB0};
    for (int i = 0; i < 2; i++)
    {
        {
            for (int j = 0; j < 2; j++)
            {
                ATADiskReader reader((ata_disk)i, j == 0);

                io_out8(::get_port((ata_disk)i, ata_port_type::drive_read), pread[j]); // master
                io_out8(::get_port((ata_disk)i, ata_port_type::selector_count), 0);
                io_out8(::get_port((ata_disk)i, ata_port_type::selector_number), 0);
                io_out8(::get_port((ata_disk)i, ata_port_type::cylinder_low), 0);
                io_out8(::get_port((ata_disk)i, ata_port_type::cylinder_high), 0);
                io_out8(::get_port((ata_disk)i, ata_port_type::status_command), 0xEC);
                u8 value = io_in8(::get_port((ata_disk)i, ata_port_type::status_command));
                if (value == 0 || value == 255)
                {
                    continue; // not exist
                }
                value = io_in8(::get_port((ata_disk)i, ata_port_type::status_command));
                while ((value & 0x80) != 0)
                    value = io_in8(::get_port((ata_disk)i, ata_port_type::status_command));
                u8 low = io_in8(::get_port((ata_disk)i, ata_port_type::cylinder_low));
                u8 high = io_in8(::get_port((ata_disk)i, ata_port_type::cylinder_high));
                if (low == 0 && high == 0)
                {
                    // is ATA
                    reader.drq_wait();
                    u8 err = reader.error_code();
                    if (err == 0)
                    {
                        reader.read_data(nullptr, 512);
                        if (reader.has_data())
                        {
                            continue; // error
                        }
                        return New<ATADiskReader>((ata_disk)i, j == 0);
                    }
                }
            }
        }
    }
    return nullptr;
}