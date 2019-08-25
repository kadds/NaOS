#include "loader/MBR.hpp"
#include "loader/ATADiskReader.hpp"
#include "loader/ScreenPrinter.hpp"
MBR::MBR(ATADiskReader *disk_reader)
{
    has_err = false;
    if (disk_reader->read_data_lba48(&header, 0, 440, sizeof(header)) == -1)
    {
        gPrinter->printf("Can not read MBR data from ATA disk! error code %d", disk_reader->error_code());
        has_err = true;
    }
}