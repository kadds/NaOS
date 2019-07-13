#include "loader/GPT.hpp"
#include "loader/ATADiskReader.hpp"
#include "loader/ScreenPrinter.hpp"
#include "loader/loader.hpp"
GPT::GPT(ATADiskReader *disk_reader) : reader(disk_reader)
{
    has_err = false;
    if (disk_reader->read_data_lba48(&header, 1, 0, sizeof(gpt_header)) == -1)
    {
        has_err = true;
        gPrinter->printf("Can not read GPT header, error code %d", disk_reader->error_code());
        return;
    }
    gPrinter->printf("GPT header %s\n", &header.signature);

    tables = (gpt_table *)alloc(header.size_of_single_partition_entry * header.number_of_partition, alignof(gpt_table));
    if (disk_reader->read_data_lba48(tables, 2, 0,
                                     header.size_of_single_partition_entry * header.number_of_partition) == -1)
    {
        has_err = true;
        gPrinter->printf("Can not read GPT tables, error code %d", disk_reader->error_code());
        return;
    }
}
partition GPT::get_partition(int index)
{
    u32 start = tables[index].start_lba;
    // reverse_endian(&start);
    u32 end = tables[index].end_lba;
    // reverse_endian(&end);

    return partition(start, end);
}