#pragma once
#include "common.hpp"

class ATADiskReader;
#pragma pack(push, 1)
struct mbr_partition_table
{
    u8 flag;
    u8 start_header;
    u8 start_selector;
    u8 start_cylinder;
    u8 file_system;
    u8 end_header;
    u8 end_selector;
    u8 end_cylinder;
    u32 rety_selector;
    u32 rety_selector_count;
};

struct mbr_header
{
    u32 flag;
    u16 null;
    mbr_partition_table partions[4];
};
struct partition
{
    u64 lba_end;
    u64 lba_start;
    bool is_valid() { return lba_end > lba_start && lba_start > 0; }

    partition(u64 lba_start = 0, u64 lba_end = 0)
        : lba_end(lba_end)
        , lba_start(lba_start)
    {
    }
};

#pragma pack(pop)
class MBR
{
    mbr_header header;
    ATADiskReader *reader;
    bool has_err;

  public:
    MBR(ATADiskReader *reader);
    ~MBR(){};

    bool isGPT() { return header.partions[0].file_system == 0xEE; }
    partition get_partition(int index)
    {
        auto &p = header.partions[index];
        return partition(p.rety_selector, p.rety_selector_count + p.rety_selector);
    }
    int partition_count() { return 1; }
    bool available() const { return !has_err; }
};
