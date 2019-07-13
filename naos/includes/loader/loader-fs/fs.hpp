#pragma once
#include "common.hpp"
#include "../MBR.hpp"

class fs
{
protected:
    ATADiskReader * reader;
public:
    fs(ATADiskReader * reader): reader(reader) {};
    virtual ~fs(){};

    // when buffer == nullptr return bytes of file
    // return -1 where error occurred
    virtual int read_file(const char * url, void * buffer) = 0;
    virtual int try_open_partition(partition partition) = 0;
};
