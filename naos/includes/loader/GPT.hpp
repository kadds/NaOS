#pragma once
#include "MBR.hpp"
#include "common.hpp"

#pragma pack(push, 1)
struct gpt_header
{
    char signature[8];
    u32 version;
    u32 header_size;
    u32 crc32;
    u32 reserved0;
    u64 current_LBA;
    u64 backup_LBA;
    u64 first_usable_LBA;
    u64 last_usable_LBA;
    char guid[16];
    u64 array_start_LBA;
    u32 number_of_partition;
    u32 size_of_single_partition_entry;
    u32 partition_crc32;
};

struct gpt_table
{
    char type_guid[16];
    char guid[16];
    u64 start_lba;
    u64 end_lba;
    u64 prop;
    char name[72];
};

#pragma pack(pop)

class GPT
{
  private:
    bool has_err;
    gpt_header header;
    gpt_table *tables;

  public:
    GPT(ATADiskReader *render);
    ~GPT(){};
    partition get_partition(int index);
    int partition_count() { return header.number_of_partition; }
    bool available() const { return !has_err; }
};
