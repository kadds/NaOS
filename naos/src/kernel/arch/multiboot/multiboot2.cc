#include "kernel/arch/multiboot/multiboot2.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/common.hpp"
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

/// Single pass over the loader tags building the {token -> range} table of
/// self-declaring service modules. Unknown tokens are retained so the kernel
/// can diagnose them without assigning them filesystem semantics.
u64 Unpaged_Text_Section find_named_modules(multiboot_tag *tags, named_boot_module *modules)
{
    u64 count = 0;
    u32 next_size = ((tags->size + 7) & ~7);
    for (; tags->type != MULTIBOOT_TAG_TYPE_END; tags = (multiboot_tag *)((u8 *)tags + next_size))
    {
        if (tags->type == MULTIBOOT_TAG_TYPE_MODULE && count < max_named_boot_modules)
        {
            multiboot_tag_module *md = (multiboot_tag_module *)tags;
            if (md->cmdline[0] != '\0')
            {
                named_boot_module &entry = modules[count++];
                u32 i = 0;
                for (; i < sizeof(entry.name) - 1 && md->cmdline[i] != '\0'; i++)
                {
                    entry.name[i] = md->cmdline[i];
                }
                entry.name[i] = '\0';
                entry.start = (u64)md->mod_start;
                entry.size = (u64)(md->mod_end - md->mod_start);
            }
        }
        next_size = ((tags->size + 7) & ~7);
    }
    return count;
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
        args->rsdp = (u64)&acpi->rsdp;
    }
}
void Unpaged_Text_Section set_args_rsdp_old(kernel_start_args *args, multiboot_tag *tags)
{
    multiboot_tag_old_acpi *acpi = (multiboot_tag_old_acpi *)tags;
    if (acpi->size - sizeof(multiboot_tag_old_acpi) > 0)
    {
        args->rsdp_old = (u64)&acpi->rsdp;
    }
}
void Unpaged_Text_Section set_args_efi(kernel_start_args *args, multiboot_tag *tags)
{
    multiboot_tag_efi64 *efi = (multiboot_tag_efi64 *)tags;
    args->efi_system_table = efi->pointer;
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
    funcs[MULTIBOOT_TAG_TYPE_ACPI_OLD] = set_args_rsdp_old;
    funcs[MULTIBOOT_TAG_TYPE_EFI64] = set_args_efi;

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
    // One pass builds the {token -> range} table of self-declaring service
    // modules; launchers later query it by name.
    // Do not value-initialize this stack buffer here.  The compiler lowers
    // `T buffer[N] = {}` to a call to the normal memset implementation, which
    // lives in the high-half kernel text.  At this point Multiboot is still
    // running with the low identity-only page table, so that call would fetch
    // from an unmapped high-half address and triple fault.  find_named_modules
    // initializes every entry that it returns, and entries beyond its count
    // are never consumed.
    named_boot_module modules[max_named_boot_modules];
    const u64 module_count = find_named_modules(tags, modules);
    // The boot data region (kernel args, relocated modules, boot allocator)
    // must never overlap any loader-provided module, so placement considers
    // the union of all module ranges.
    u32 mod_start = 0xffffffff;
    u32 mod_end = 0;
    for (u64 i = 0; i < module_count; i++)
    {
        const u32 mstart = (u32)modules[i].start;
        const u32 mend = (u32)(modules[i].start + modules[i].size);
        if (mstart < mod_start)
            mod_start = mstart;
        if (mend > mod_end)
            mod_end = mend;
    }
    u32 offset;
    if (mod_end <= 0x100000) // lower 1MB
    {
        if (mod_end >= 0x80000) // page table and stack protection
        {
            panic();
        }
        offset = (u64)_bss_end;
    }
    else if ((u32)(u64)start_ptr < mod_start)
    {
        offset = (u64)mod_end;
    }
    else
    {
        offset = (u64)max_boot_tag_ptr;
    }

    // The boot arguments and the unpaged allocator are written before the
    // normal memory initializer can relocate modules.  max_boot_tag_ptr is
    // not necessarily outside the loader-provided modules (GRUB may place
    // the tags between module payloads), so the old branch above could make
    // alloca_data() overwrite a module header before it was copied.  Keep
    // the existing placement when it is already past the module union, but
    // move the whole boot-data area past that union whenever the candidate
    // is below it. This is the minimum lifetime guarantee needed by named
    // boot modules.
    if (module_count != 0 && offset < mod_end)
    {
        offset = (u64)mod_end;
    }

    // align 4Kib
    offset = (offset + 0x1000 - 1) & ~(0x1000 - 1);
    data_offset = offset;
    kernel_start_args *args = (kernel_start_args *)alloca_data(sizeof(kernel_start_args), 8);

    for (u64 i = 0; i < module_count; i++)
    {
        args->named_modules[i] = modules[i];
    }
    args->named_module_count = module_count;
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
