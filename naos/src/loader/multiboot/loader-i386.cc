
#include "loader/ATADiskReader.hpp"
#include "loader/GPT.hpp"
#include "loader/ScreenPrinter.hpp"
#include "loader/loader-fs/fat.hpp"
#include "loader/loader.hpp"
#include "loader/multiboot.hpp"
#include "loader/util.hpp"

extern volatile char _bss_end_addr[];

extern volatile int _stack_size[];
void *alloc_data(u64 &data_ptr, u32 size, u32 align);

char *head_start;
u32 head_allco_size = 0;
// http://www.gnu.org/software/grub/manual/multiboot/multiboot.html#Examples
#define CHECK_FLAG(flags, bit) ((flags) & (1 << (bit)))

ExportC void _main(unsigned int magic, multiboot_info_t *addr)
{
    static_init();
    head_start = (char *)(_bss_end_addr + *_stack_size);
    ScreenPrinter printer(addr->framebuffer_width, addr->framebuffer_height);
    gPrinter = &printer;
    printer.cls();
    printer.printf("Header_data_addr = 0x%x, stack_size = %dKb\n", ((u32)head_start), (*_stack_size) >> 10);

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
    {
        printer.printf("Invalid magic number: 0x%x.\n", (unsigned)magic);
        return;
    }
    printer.printf("Load from multiboot.\n");

    ATADiskReader *disk_reader = ATADiskReader::get_available_disk();
    if (disk_reader == nullptr)
    {
        printer.printf("Can not load ata disk. \n");
        return;
    }
    disk_reader->reset();
    MBR *mbr = New<MBR>(disk_reader);
    if (!mbr->available())
        return;
    void *raw_kernel;
    int size = 0;
    if (mbr->isGPT())
    {
        GPT *gpt = New<GPT>(disk_reader);
        if (gpt->available())
        {
            int pc = gpt->partition_count();
            for (int i = 0; i < pc; i++)
            {
                auto cur_partition = gpt->get_partition(i);
                Fat *fat = New<Fat>(disk_reader);
                if (cur_partition.is_vaild())
                {
                    printer.printf("Partition %d is vaild. start_lba at: %u%u, end_lba at %u%u.\n", i,
                                   (u32)(cur_partition.lba_start >> 32), (u32)cur_partition.lba_start,
                                   (u32)(cur_partition.lba_end >> 32), (u32)cur_partition.lba_end);
                    if (fat->try_open_partition(cur_partition) >= 0)
                    {
                        printer.printf("Partition %d is fat file system. Try loading kernel ...\n", i);
                        size = fat->read_file("/SYSTEM/KERNEL", raw_kernel);
                        if (size > 0)
                        {
                            break;
                        }
                    }
                }
            }
        }
        else
            return;
    }
    if (size == 0)
    {
        printer.printf("Can not load any kernel file!\n");
        return;
    }
    const u64 base_kernel_ptr = 0x100000;
    u64 base_data_ptr = base_kernel_ptr + size, current_data_ptr;

    memcopy((void *)base_kernel_ptr, raw_kernel, size);
    kernel_file_head_t *kernel = (kernel_file_head_t *)base_kernel_ptr;
    printer.printf("kernel size: %u\n", size);
    base_data_ptr += kernel->reserved_space_size;

    base_data_ptr = ((base_data_ptr + 16 - 1) & ~(16 - 1));
    current_data_ptr = base_data_ptr;

    kernel_start_args *args =
        (kernel_start_args *)alloc_data(current_data_ptr, sizeof(kernel_start_args), alignof(kernel_start_args));
    args->kernel_base = base_kernel_ptr;
    args->kernel_size = size;
    args->stack_base = 0x7000;
    args->stack_size = 0x1000;
    args->data_base = base_data_ptr;
    args->data_size = 0x8;

    if (CHECK_FLAG(addr->flags, 6))
    {
        args->mmap_count = (addr->mmap_length) / 24;
        if (addr->mods_count > 128)
        {
            printer.printf("To many memory map items.\n");
            return;
        }
        args->set_mmap_ptr((kernel_memory_map_item *)alloc_data(
            current_data_ptr, sizeof(kernel_memory_map_item) * args->mmap_count, alignof(kernel_memory_map_item)));
        for (u32 i = 0; i < args->mmap_count; i++)
        {
            auto &ptr = *(((multiboot_memory_map_t *)addr->mmap_addr) + i);
            auto &target = args->get_mmap_ptr()[i];
            target.addr = ptr.addr;
            target.len = ptr.len;
            switch (ptr.type)
            {
                case MULTIBOOT_MEMORY_AVAILABLE:
                    target.map_type = map_type_t::available;
                    break;
                case MULTIBOOT_MEMORY_RESERVED:
                    target.map_type = map_type_t::reserved;
                    break;
                case MULTIBOOT_MEMORY_ACPI_RECLAIMABLE:
                    target.map_type = map_type_t::acpi;
                    break;
                case MULTIBOOT_MEMORY_NVS:
                    target.map_type = map_type_t::nvs;
                    break;
                case MULTIBOOT_MEMORY_BADRAM:
                    target.map_type = map_type_t::badram;
                    break;
                default:
                    printer.printf("Unknown memory map type %d.\n", ptr.type);
                    return;
            }
        }
    }
    else
    {
        printer.printf("Can not load memory map info.\n");
    }
    if (!_is_support_x64())
    {
        printer.printf("Unsupport x64 mode\n");
        return;
    }
    printer.printf("Enter ia-32e & Start kernel...\n");
    args->data_size = current_data_ptr - base_data_ptr;
    run_kernel((void *)(kernel->start_addr + base_kernel_ptr), args);
}

void *alloc_data(u64 &data_ptr, u32 size, u32 align)
{
    char *start = (char *)(((u64)data_ptr + align - 1) & ~(align - 1));
    data_ptr = (u64)start + size;
    return start;
}

void *alloc(u32 size, u32 align)
{
    char *start = (char *)(((u32)head_start + align - 1) & ~(align - 1));
    head_allco_size += size;
    head_start = start + size;
    return start;
}