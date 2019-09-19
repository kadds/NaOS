#pragma once
#include "common.hpp"
#include "kernel/mm/allocator.hpp"

typedef u32 uchar;

class string
{
  private:
    byte *buffer;
    u64 size;
    memory::IAllocator *allocator;

  public:
    static string from_utf8(memory::IAllocator, const char *str);
    static string from_uft16(memory::IAllocator, const char *str);

    uchar operator[](u64 index) { return 0; }
};
