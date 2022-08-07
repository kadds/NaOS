#pragma once
#include "common.hpp"
#define Unpaged_Text_Section Section(".unpaged.text")
#define Unpaged_Data_Section(i) Section(".unpaged.data." #i)
#define Unpaged_Bss_Section Section(".unpaged.bss")
#define Trace_Section

static_assert(sizeof(void *) == 8, "Just 64-bit code generation is supported.");

/// Show memory space type
enum class map_type_t : u64
{
    available = 0, ///< This is the memory space that the kernel can use.
    reserved,
    acpi,
    nvs,
    badram
};

struct kernel_memory_map_item
{
    u64 addr;
    u64 len;
    map_type_t map_type;
};

/// The args passed by loaders
///
/// including kernel switches, video infomation, root file system
/// address
struct kernel_start_args
{
  public:
    u64 size_of_struct; ///< Struct size. Ready for expansion
    u64 kernel_base;
    u64 kernel_size;
    u64 data_base; ///< Always placed after the kernel image
    u64 data_size;
    u64 fb_addr; ///< Linear frame buffer physical address, 0: does not exist video device
    u32 fb_pitch;
    u32 fb_width;
    u32 fb_height;
    u32 fb_bbp;
    u32 fb_type;    ///< 2: text mode, 1: graphics mode, 0: index mode
    u64 mmap;       ///< Pointer, the memory map address
    u64 mmap_count; ///< The memory map count

    u64 rfsimg_start; ///< Root file system image address, must be aligned to page size
    u64 rfsimg_size;  ///< Root file system image size

    u64 command_line;     ///< Pointer, kernel boot command string
    u64 boot_loader_name; ///< Pointer, like "grub2", "efi" string

    u64 rsdp; ///< Pointer, ACPI RSDP
} PackStruct;

/// Kernel file struct
///
/// Parsed by loader.
/// View head.S
struct kernel_file_head_t
{
    u64 magic0;              ///< Must be 0xEEFFFFEE00FFFF00
    u64 start_addr;          ///< Kernel start function address. Link to head.S(_start)
    u64 reserved_space_size; ///< Show reserved size for bss
    u64 magic1;              ///< Must be 0xAA5555AAAA0000AA
    bool is_valid() { return magic0 == 0xEEFFFFEE00FFFF00UL && magic1 == 0xAA5555AAAA0000AAUL; }
} PackStruct;

#define _ctx_interrupt_
ExportC NoReturn void _kstart(kernel_start_args *args);

ExportC Unpaged_Text_Section void _init_unpaged(const kernel_start_args *args);

extern kernel_start_args *kernel_args;
extern u64 timestamp_version;