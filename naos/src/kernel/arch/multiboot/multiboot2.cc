#include "kernel/arch/multiboot/multiboot2.hpp"
#include "common.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/kernel.hpp"
Unpaged_Bss_Section u32 data_offset;
void Unpaged_Text_Section *alloca(u32 size, u32 align)
{
    char *start = (char *)(((u64)data_offset + align - 1) & ~(align - 1));
    data_offset = (u64)start + size;
    return start;
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
Unpaged_Data_Section const char rfsimage[] = "rfsimg";

ExportC void Unpaged_Text_Section _multiboot_main(void *header)
{
    if (header == nullptr)
        while (1)
            ;
    u32 len = *(u32 *)header;
    multiboot_tag *tags = (multiboot_tag *)((byte *)header + 8);
    data_offset = (u64)header + len;
    kernel_start_args *args = (kernel_start_args *)alloca(sizeof(kernel_start_args), 8);

    args->mmap_count = 0;
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
            case MULTIBOOT_TAG_TYPE_MODULE: {
                multiboot_tag_module *md = (multiboot_tag_module *)tags;
                if (strcmp((char *)md->cmdline, rfsimage) == 0)
                {
                    args->rfsimg_start = (u64)md->mod_start;
                    args->rfsimg_size = (md->mod_end - md->mod_start);
                }
                break;
            }
        }
    }
    args->size_of_struct = sizeof(kernel_start_args);
    args->kernel_base = (u64)base_phy_addr;
    args->kernel_size = (u64)_bss_end;
    args->data_base = (u64)_bss_end;

    u64 rsp = (u64)base_virtual_addr;

    args->data_size = (u32)data_offset - (u32)args->data_base;
    _init_unpaged(args);
    __asm__ __volatile__("addq	%0,	%%rsp	\n\t" : : "r"(rsp) : "memory");
    args = (kernel_start_args *)((byte *)args + rsp);
    _kstart(args);
    while (1)
        ;
}
