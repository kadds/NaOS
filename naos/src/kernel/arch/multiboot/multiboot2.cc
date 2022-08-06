#include "kernel/arch/multiboot/multiboot2.hpp"
#include "common.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/kernel.hpp"
Unpaged_Bss_Section u32 data_offset;
Unpaged_Bss_Section void *max_boot_tag_ptr;

void *Unpaged_Text_Section alloca_data(u32 size, u32 align)
{
    u32 start = (data_offset + align - 1) & ~(align - 1);
    data_offset = (u32)start + size;
    // memzero
    for (u32 i = 0; i < size; i++)
    {
        char *p = (char *)(u64)(start + i);
        *p = 0;
    }
    return (void *)(u64)start;
}

int Unpaged_Text_Section strcmp_2(const char *str1, const char *str2)
{
    while (1)
    {
        char s1, s2;
        s1 = *str1++;
        s2 = *str2++;
        if (s1 != s2)
            return s1 < s2 ? -1 : 1;
        if (!s1)
            break;
    }
    return 0;
}

int Unpaged_Text_Section strlen_2(const char *str)
{
    int i = 0;
    while (*str++ != 0)
        i++;
    return i++;
}

void Unpaged_Text_Section memcpy_2(void *dst, const void *source, u32 len)
{
    char *d = (char *)dst;
    const char *s = (const char *)source;
    for (u32 i = 0; i < len; i++)
    {
        *d++ = *s++;
    }
}

Unpaged_Data_Section const char rfsimage[] = "";

bool Unpaged_Text_Section find_rfsimage(multiboot_tag *tags, u32 *start, u32 *end)
{
    u32 next_size = ((tags->size + 7) & ~7);
    for (; tags->type != MULTIBOOT_TAG_TYPE_END; tags = (multiboot_tag *)((u8 *)tags + next_size))
    {
        if (tags->type == MULTIBOOT_TAG_TYPE_MODULE)
        {
            multiboot_tag_module *md = (multiboot_tag_module *)tags;
            if (strcmp_2((char *)md->cmdline, rfsimage) == 0)
            {
                *start = (u64)md->mod_start;
                *end = (u64)md->mod_end;
                return true;
            }
        }
        next_size = ((tags->size + 7) & ~7);
    }
    return false;
}

NoReturn void Unpaged_Text_Section panic()
{
    while (1)
    {
        __asm__ __volatile__("pause\n\t" : : : "memory");
    }
}

void Unpaged_Text_Section set_args_mmap(kernel_start_args *args, multiboot_tag *tags)
{
    multiboot_memory_map_t *mmap = ((multiboot_tag_mmap *)tags)->entries;
    u32 entry_len = ((multiboot_tag_mmap *)tags)->entry_size;
    u32 count = ((multiboot_tag_mmap *)tags)->size / entry_len;
    args->mmap_count = count;
    auto *map = (kernel_memory_map_item *)alloca_data(args->mmap_count * sizeof(kernel_memory_map_item), 8);
    args->mmap = (u64)map;

    for (u32 i = 0; i < count; i++, mmap = (multiboot_memory_map_t *)((byte *)mmap + entry_len))
    {
        map[i].addr = mmap->addr;
        map[i].len = mmap->len;
        map_type_t t = (map_type_t)(mmap->type - 1);
        map[i].map_type = t;
    }
}

void Unpaged_Text_Section set_args_fb(kernel_start_args *args, multiboot_tag *tags)
{
    multiboot_tag_framebuffer *fb = (multiboot_tag_framebuffer *)tags;
    args->fb_addr = fb->common.framebuffer_addr;
    args->fb_width = fb->common.framebuffer_width;
    args->fb_height = fb->common.framebuffer_height;
    args->fb_bbp = fb->common.framebuffer_bpp;
    args->fb_type = fb->common.framebuffer_type;
    args->fb_pitch = fb->common.framebuffer_pitch;
}

void Unpaged_Text_Section set_args_cmdline(kernel_start_args *args, multiboot_tag *tags)
{
    multiboot_tag_string *str = (multiboot_tag_string *)tags;
    int str_len = strlen_2(str->string);
    void *p = alloca_data(str_len + 1, 1);
    memcpy_2(p, str->string, str_len);
    args->command_line = (u64)p;
}

void Unpaged_Text_Section set_args_boot(kernel_start_args *args, multiboot_tag *tags)
{
    multiboot_tag_string *str = (multiboot_tag_string *)tags;
    int str_len = strlen_2(str->string);
    void *p = alloca_data(str_len + 1, 1);
    memcpy_2(p, str->string, str_len);
    args->boot_loader_name = (u64)p;
}

void Unpaged_Text_Section set_args_rsdp(kernel_start_args *args, multiboot_tag *tags)
{
    multiboot_tag_new_acpi *acpi = (multiboot_tag_new_acpi *)tags;
    if (acpi->size - sizeof(multiboot_tag_new_acpi) > 0)
    {
        args->rsdp = acpi->rsdp[0];
    }
}

typedef void (*set_args_func)(kernel_start_args *args, multiboot_tag *tags);

Unpaged_Bss_Section set_args_func funcs[22];

void Unpaged_Text_Section set_args(kernel_start_args *args, multiboot_tag *tags)
{
    funcs[MULTIBOOT_TAG_TYPE_CMDLINE] = set_args_cmdline;
    funcs[MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME] = set_args_boot;
    funcs[MULTIBOOT_TAG_TYPE_MMAP] = set_args_mmap;
    funcs[MULTIBOOT_TAG_TYPE_FRAMEBUFFER] = set_args_fb;
    funcs[MULTIBOOT_TAG_TYPE_ACPI_NEW] = set_args_rsdp;

    u32 next_size = ((tags->size + 7) & ~7);
    for (; tags->type != MULTIBOOT_TAG_TYPE_END; tags = (multiboot_tag *)((u8 *)tags + next_size))
    {
        if (tags->type <= 21)
        {
            if (funcs[tags->type] != 0)
            {
                funcs[tags->type](args, tags);
            }
        }
        next_size = ((tags->size + 7) & ~7);
    }
}

/// disable jump table
ExportC u64 Unpaged_Text_Section _multiboot_main(void *header, u64 *kstart, u64 *rsp0)
{
    if (header == nullptr)
        panic();
    u32 start_ptr = (u32)(u64)header;
    max_boot_tag_ptr = (void *)(*(u32 *)header + ((u8 *)header));
    multiboot_tag *tags = (multiboot_tag *)((byte *)header + 8);
    u32 start, end;
    if (!find_rfsimage(tags, &start, &end))
        panic();
    u32 offset;
    if (end <= 0x100000) // lower 1MB
    {
        if (end >= 0x80000) // page table and stack protection
        {
            panic();
        }
        offset = (u64)_bss_end;
    }
    else if ((u32)(u64)start_ptr < start) 
    {
        offset = (u64)end;
    }
    else
    {
        offset = (u64)max_boot_tag_ptr;
    }
    
    // align 4Kib
    offset = (offset + 0x1000 - 1) & ~(0x1000 - 1);
    data_offset = offset;
    kernel_start_args *args = (kernel_start_args *)alloca_data(sizeof(kernel_start_args), 8);

    args->rfsimg_start = (u64)start;
    args->rfsimg_size = end - start;
    args->data_base = offset;
    set_args(args, tags);

    args->size_of_struct = sizeof(kernel_start_args);
    args->kernel_base = (u64)base_phy_addr;
    args->kernel_size = (u64)_bss_end - (u64)base_phy_addr;

    args->data_size = data_offset - args->data_base;
    _init_unpaged(args);
    args = (kernel_start_args *)((byte *)args + memory::linear_addr_offset);
    *kstart = (u64)_kstart;
    *rsp0 = (u64)memory::kernel_cpu_stack_rsp0;
    return (u64)args;
}
