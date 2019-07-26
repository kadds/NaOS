#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _MSC_VER
#define ExportC extern "C" __declspec(dllexport)
#endif

#if defined __GNUC__ || defined __clang__
#define ExportC extern "C"
#endif

#define PackStruct __attribute__((packed))
#define Aligned(v) __attribute__((aligned(v)))
#define Section(v) __attribute__((section(v)))
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

enum class map_type_t
{
    available = 0,
    reserved,
    acpi,
    nvs,
    badram
};
struct kernel_memory_map_item
{
    u32 size;
    u64 addr;
    u64 len;
    map_type_t map_type;
};
struct kernel_start_args
{
  public:
    u64 kernel_base;
    u64 kernel_size;
    u64 stack_base;
    u64 stack_size;
    u64 data_base;
    u64 data_size;

  private:
    u64 mmap;

  public:
    u32 mmap_count;
    void set_mmap_ptr(kernel_memory_map_item *item_ptr) { mmap = (u64)item_ptr; }
    kernel_memory_map_item *get_mmap_ptr() { return (kernel_memory_map_item *)mmap; }
} PackStruct;

struct kernel_file_head_t
{
    u64 start_addr;
    u64 bss_end_addr;
} PackStruct;

typedef void (*InitFunc)(void);
ExportC InitFunc __init_array_start;
ExportC InitFunc __init_array_end;
inline void static_init()
{
    InitFunc *pFunc = &__init_array_start;

    for (; pFunc < &__init_array_end; ++pFunc)
    {
        (*pFunc)();
    }
}

