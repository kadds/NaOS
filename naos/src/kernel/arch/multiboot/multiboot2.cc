#include "kernel/arch/multiboot/multiboot2.hpp"
#include "common.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/kernel.hpp"
Unpaged_Bss_Section u32 data_offset;
void *Unpaged_Text_Section alloca(u32 size, u32 align)
{
    u32 start = (data_offset + align - 1) & ~(align - 1);
    data_offset = (u32)start + size;
    return (void *)(u64)start;
}
int Unpaged_Text_Section strcmp(const char *str1, const char *str2)
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

int Unpaged_Text_Section strlen(const char *str)
{
    int i = 0;
    while (*str++ != 0)
        i++;
    return i++;
}

void Unpaged_Text_Section memcpy(void *dst, const void *source, u32 len)
{
    char *d = (char *)dst;
    const char *s = (const char *)source;
    for (u32 i = 0; i < len; i++)
    {
        *d++ = *s++;
    }
}

Unpaged_Data_Section const char rfsimage[] = "rfsimg";

bool Unpaged_Text_Section find_rfsimage(multiboot_tag *tags, u32 *start, u32 *end)
{
    for (; tags->type != MULTIBOOT_TAG_TYPE_END; tags = (multiboot_tag *)((u8 *)tags + ((tags->size + 7) & ~7)))
    {
        if (tags->type == MULTIBOOT_TAG_TYPE_MODULE)
        {
            multiboot_tag_module *md = (multiboot_tag_module *)tags;
            if (strcmp((char *)md->cmdline, rfsimage) == 0)
            {
                *start = (u64)md->mod_start;
                *end = (u64)md->mod_end;
                return true;
            }
        }
    }
    return false;
}
void Unpaged_Text_Section panic()
{
    while (1)
    {
        __asm__ __volatile__("hlt\n\t" : : : "memory");
    }
}

ExportC void Unpaged_Text_Section _multiboot_main(void *header)
{
    if (header == nullptr)
        panic();
    u32 len = *(u32 *)header;
    multiboot_tag *tags = (multiboot_tag *)((byte *)header + 8);
    u32 start, end;
    if (!find_rfsimage(tags, &start, &end))
        panic();
    u32 offset;
    if (end <= 0x1000000) // in lower 16 MB
    {
        offset = end;
    }
    else if ((u64)header + len < start - 0x1000) // 1KB data
    {
        offset = (u64)header + len;
    }
    else
        panic();

    offset = (offset + 0x1000 - 1) & ~(0x1000 - 1);
    data_offset = offset;
    kernel_start_args *args = (kernel_start_args *)alloca(sizeof(kernel_start_args), 8);
    args->rfsimg_start = (u64)start;
    args->rfsimg_size = end - start;
    args->data_base = offset;

    for (; tags->type != MULTIBOOT_TAG_TYPE_END; tags = (multiboot_tag *)((u8 *)tags + ((tags->size + 7) & ~7)))
    {
        switch (tags->type)
        {
            case MULTIBOOT_TAG_TYPE_MMAP: {
                multiboot_memory_map_t *mmap = ((multiboot_tag_mmap *)tags)->entries;
                u32 len = ((multiboot_tag_mmap *)tags)->entry_size;
                u32 count = ((multiboot_tag_mmap *)tags)->size / len;
                args->mmap_count = count;
                auto *map = (kernel_memory_map_item *)alloca(args->mmap_count * sizeof(kernel_memory_map_item), 8);
                args->mmap = (u64)map;

                for (u32 i = 0; i < count; i++, mmap = (multiboot_memory_map_t *)((byte *)mmap + len))
                {
                    map[i].addr = mmap->addr;
                    map[i].len = mmap->len;
                    map_type_t t = (map_type_t)(mmap->type - 1);
                    map[i].map_type = t;
                }
                break;
            }
            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
                multiboot_tag_framebuffer *fb = (multiboot_tag_framebuffer *)tags;
                args->fb_addr = fb->common.framebuffer_addr;
                args->fb_width = fb->common.framebuffer_width;
                args->fb_height = fb->common.framebuffer_height;
                args->fb_bbp = fb->common.framebuffer_bpp;
                args->fb_type = fb->common.framebuffer_type;
                args->fb_pitch = fb->common.framebuffer_pitch;
                break;
            }
            case MULTIBOOT_TAG_TYPE_CMDLINE: {
                multiboot_tag_string *str = (multiboot_tag_string *)tags;
                int len = strlen(str->string);
                void *p = alloca(len, 1);
                memcpy(p, str->string, len);
                args->command_line = (u64)p;
                break;
            }
            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME: {
                multiboot_tag_string *str = (multiboot_tag_string *)tags;
                int len = strlen(str->string);
                void *p = alloca(len, 1);
                memcpy(p, str->string, len);
                args->boot_loader_name = (u64)p;
                break;
            }
        }
    }

    args->size_of_struct = sizeof(kernel_start_args);
    args->kernel_base = (u64)base_phy_addr;
    args->kernel_size = (u64)_bss_end - (u64)base_phy_addr;

    u64 rsp = (u64)base_virtual_addr;

    args->data_size = data_offset - args->data_base;
    _init_unpaged(args);
    __asm__ __volatile__("addq	%0,	%%rsp	\n\t" : : "r"(rsp) : "memory");
    args = (kernel_start_args *)((byte *)args + rsp);
    _kstart(args);
    while (1)
        ;
}
