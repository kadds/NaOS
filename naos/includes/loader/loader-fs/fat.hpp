#pragma once
#include "fs.hpp"

#pragma pack(push, 1)
struct fat16_header_ext
{
    u8 number_of_driver;
    u8 current_track;
    u8 signature;
    u32 id;
    char label[11];
    char fs_type[8];
    char bootloader[448];
};

struct fat32_header_ext
{
    u32 selector_per_fat;

    u16 flags;
    u16 version;
    u32 root_cluster;
    u16 fsinfo;
    u16 backup;
    u8 reserved[12];

    u8 number_of_driver;
    u8 current_track;
    u8 signature;
    u32 id;
    char label[11];
    char fs_type[8];
    char bootloader[420];
};
struct fat_header
{
    u8 jmp[3];
    char oem_name[8];
    u16 bytes_per_selector;
    u8 selector_per_cluster;
    u16 reserved_selectors;
    u8 number_of_fat;
    u16 max_root_count;
    u16 selector_all_count_16;
    u8 description;
    u16 selector_per_fat;
    u16 selector_per_track;
    u16 number_of_track_heads;
    u32 hidden_selector;
    u32 selector_all_count_32;

    union
    {
        fat16_header_ext fat16;
        fat32_header_ext fat32;
    };

    u16 ends;
};

struct fat_link
{
};

struct short_file_entry
{
    char name[8];
    char ext[3];
    u8 attribute;
    u8 reserved0;
    u8 create_time_resolution;
    u16 create_time;
    u16 create_date;
    u16 recent_date;
    u16 first_cluster_high;
    u16 last_modify_time;
    u16 last_modify_date;
    u16 first_cluster_low;
    u32 file_size;
};
struct long_file_entry
{
    u8 ord;
    char name1[10];
    u8 attribute;
    u8 type;
    u8 checksum;
    char name2[12];
    u16 first_cluster;
    char name3[4];
};
struct file_entry
{
    union
    {
        short_file_entry short_file;
        long_file_entry long_file;
    };
};
struct fs_info
{
    u32 lead_sig;
    char reserved0[480];
    u32 struct_sig;
    u32 free_count;
    u32 next_free;
    char reserved1[12];
    u32 trail_sig;
};

#pragma pack(pop)

class Fat
{
  private:
    ATADiskReader *reader;
    partition partition_lbas;
    fat_header header;
    u64 root_start;
    u64 fat_start;

    u32 current_fat_selector_index;
    u32 *fat_cache_data;
    u32 get_fat(u32 cluster);
    void load_fat(u32 selector_index);

  public:
    Fat(ATADiskReader *reader)
        : reader(reader){};
    ~Fat(){};
    int try_open_partition(partition partition);
    int read_file(const char *url, void *&buffer);
};
