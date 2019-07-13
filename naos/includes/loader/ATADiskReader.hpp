#pragma once
#include "common.hpp"

enum class ata_disk
{
    primary,
    secondary,
};
enum class ata_port_type
{
    data,
    error_feature,
    selector_count,
    selector_number,
    cylinder_low,
    cylinder_high,
    drive_read,
    status_command,
    control,
};

class ATADiskReader
{
    ata_disk disk;
    bool master;
    u64 bytes_read;

    bool has_data();
    int disk_wait();
    int drq_wait();
    void sleep_wait();
    void out_lba48(u64 lba, u16 lba_count);
    void read_data(void *dest, u64 byte_count);
    short get_port(ata_port_type port_type);

  public:
    ATADiskReader(ata_disk disk, bool master);
    int error_code();
    int read_data_lba48(void *dest, u64 start_lba, u32 offset_bytes, u32 size_of_bytes);
    int reset();

    static ATADiskReader *get_available_disk();
};