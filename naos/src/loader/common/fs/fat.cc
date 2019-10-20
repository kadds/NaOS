#include "loader/loader-fs/fat.hpp"
#include "loader/ATADiskReader.hpp"
#include "loader/ScreenPrinter.hpp"
#include "loader/loader.hpp"

int Fat::try_open_partition(partition ppartition)
{
    partition_lbas = ppartition;
    if (reader->read_data_lba48(&header, partition_lbas.lba_start, 0, sizeof(header)) == -1)
    {
        gPrinter->printf("can not load fat data, error code %d\n", reader->error_code());
        return -1;
    }
    if (!(header.description >= 0xFA || (header.description >= 0xED && header.description <= 0xEF) ||
          header.description == 0xE5 || header.description == 0xF0 || header.description == 0xF4 ||
          header.description == 0xF5 || header.description == 0xF8 || header.description == 0xF9))
    {
        return -2;
    }
    fs_info fs_info;
    if (header.fat32.signature != 0x28 && header.fat32.signature != 0x29)
    {
        return -2;
    }
    fat_start = header.reserved_selectors + partition_lbas.lba_start;
    root_start = fat_start + header.fat32.selector_per_fat * header.number_of_fat;

    if (reader->read_data_lba48(&fs_info, partition_lbas.lba_start + header.fat32.fsinfo, 0, sizeof(fs_info)) == -1)
    {
        gPrinter->printf("can not load fat fs info, error code %d\n", reader->error_code());
        return -1;
    }
    if (fs_info.lead_sig != 0x41615252 || fs_info.struct_sig != 0x61417272 || fs_info.trail_sig != 0xaa550000)
    {
        return -2;
    }
    fat_cache_data = (u32 *)alloc(header.bytes_per_selector, 4);
    load_fat(0);
    return 1;
}
unsigned char fat_checksum(const unsigned char *pFcbName)
{
    int i;
    unsigned char sum = 0;

    for (i = 11; i; i--)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *pFcbName++;
    return sum;
}
void Fat::load_fat(u32 selector_index)
{
    current_fat_selector_index = selector_index;
    reader->read_data_lba48(fat_cache_data, fat_start + selector_index, 0, header.bytes_per_selector);
}
u32 Fat::get_fat(u32 cluster)
{
    u32 selector = cluster / (header.bytes_per_selector / 4);
    if (current_fat_selector_index != selector)
        load_fat(selector);
    return fat_cache_data[cluster % (header.bytes_per_selector / 4)] & 0x0FFFFFFF;
}
char **split_url(const char *url, int &count)
{
    char **url_components;
    count = 0;
    int count_components[255];
    int i = 0;
    if (*url != '\0')
        url++;
    const char *temp_url = url;

    while (*temp_url != '\0')
    {
        if (*temp_url == '\\' || *temp_url == '/')
        {
            count_components[count] = i;
            i = 0;
            count++;
        }
        else
            i++;
        temp_url++;
    }

    // last : file name
    count_components[count++] = i;

    temp_url = url;
    url_components = (char **)alloc(sizeof(char *) * count, alignof(char *));

    for (i = 0; i < count; i++)
    {
        url_components[i] = (char *)alloc(sizeof(char) * count_components[i] + 1, alignof(char));
        for (int j = 0; j < count_components[i]; j++)
        {
            url_components[i][j] = *temp_url++;
        }
        temp_url++;
    }

    return url_components;
}
bool strcmp(const char *str1, const char *str2)
{
    while (*str1 != 0 && *str2 != 0)
    {
        if (*str1++ != *str2++)
            return false;
    }
    return *str1 == *str2;
}

char *get_file_name(const char *name, const char *ext)
{
    char *str = (char *)alloc(sizeof(char) * 13, 1);
    char *temp = str;
    while (*name != ' ')
        *temp++ = *name++;
    if (*ext != ' ')
        *temp++ = '.';

    while (*ext != ' ')
        *temp++ = *ext++;
    *temp = '\0';
    return str;
}

int Fat::read_file(const char *url, void *&buf)
{
    file_entry *file_entries = (file_entry *)alloc(512, alignof(file_entry));
    int url_len;
    char **urls = split_url(url, url_len);
    u32 cluster = 2;
    u32 file_size = 0;
    for (int i = 0; i < url_len; i++)
    {
        char *current_name = urls[i];
        bool searched = false;

        reader->read_data_lba48(file_entries, root_start,
                                (cluster - 2) * header.selector_per_cluster * header.bytes_per_selector,
                                header.bytes_per_selector);
        for (u16 j = 0; j < header.bytes_per_selector / sizeof(file_entry); j++)
        {
            u32 cluster_node = 0;
            file_entry &current_file = file_entries[j];
            if (current_file.long_file.attribute == 0x0F)
            {
                cluster_node = get_fat(current_file.long_file.first_cluster);
                // long file name
                continue;
            }
            else
            {
                if ((current_file.long_file.attribute & 0x10) == 0)
                {
                    // file
                    if (i + 1 != url_len)
                        continue;
                }
                else
                {
                    // directory
                    if (i + 1 == url_len)
                        continue;
                }
                cluster_node =
                    (current_file.short_file.first_cluster_low | (current_file.short_file.first_cluster_high << 16));
            }
            if (strcmp(get_file_name(current_file.short_file.name, current_file.short_file.ext), current_name))
            {
                searched = true;
                cluster = cluster_node;
                file_size = current_file.short_file.file_size;
                break;
            }
        }

        if (!searched)
        {
            gPrinter->printf("Can not find a kernel file.\n");
            return -1;
        }
    }
    // read file cluster
    char *buffer = (char *)alloc(file_size, 4);
    buf = buffer;
    u32 next_cluster = cluster;
    u32 rest_size = file_size;
    for (;;)
    {
        u32 size = rest_size > header.bytes_per_selector * header.selector_per_cluster
                       ? header.bytes_per_selector * header.selector_per_cluster
                       : rest_size;
        reader->read_data_lba48(buffer, root_start,
                                (next_cluster - 2) * header.selector_per_cluster * header.bytes_per_selector, size);
        buffer += size;
        rest_size -= size;
        next_cluster = get_fat(next_cluster);
        if (next_cluster == 0)
        {
            return -2;
        }
        if (next_cluster >= 0x0FFFFFF7 || next_cluster < 0x2)
        {
            break;
        }
    }
    return file_size;
}