#include "kernel/task/binary_handle/elf.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
#include "kernel/util/str.hpp"

namespace bin_handle
{

namespace elf_type
{
enum : u16
{
    none = 0,
    rel = 1,
    exec = 2,
    dyn = 3,
    core = 4,
    num = 5,
};
} // namespace elf_type

namespace elf_machine
{
enum : u16
{
    none,
    m32,
    sparc,
    i386,
    m68k,
    m88k,
    reserved,
    i860,
    mips,
    s370
};
} // namespace elf_machine

struct elf_header_64
{
    unsigned char ident[16];
    /// elf_type
    u16 type;
    u16 machine;
    u32 version;
    /// execute the entry point
    u64 entry;
    /// program header table offset
    u64 phoff;
    /// section header table offset
    u64 shoff;
    /// eflags
    u32 flags;
    /// header size. For 64-bit is 64
    u16 ehsize;
    /// the size of each entry in the program header table.
    u16 phentsize;
    u16 phnum;
    ///  a section header's size in bytes
    u16 shentsize;
    u16 shnum;
    /// section header string table index.
    u16 shstrndx;

} PackStruct;

namespace section_type
{
enum : u32
{
    null,
    progbits,
    symtab,
    strtag,
    rela,
    hash,
    dynamic,
    note,
    nobits,
    rel,
    shitib, ///< revesered
    dynsym,
};
} // namespace section_type

namespace program_type
{
enum : u32
{
    null,
    load,
    dynamic,
    interp,
    note,
    shlib,
    phdr,
    tls,
    gnu_eh_frame = 0x6474e550,
    gnu_stack,
    gnu_relro,
};
} // namespace program_type

struct section_64
{
    u32 name_index;
    u32 type;
    u64 flags;
    u64 addr;
    u64 offset;
    u64 size;
    u32 link;
    u32 info;
    u64 align;
    u64 entsize;
} PackStruct;

struct program_64
{
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 file_size;
    u64 mm_size;
    u64 align;
} PackStruct;

bool is_little_endian()
{
    u32 data = 0x12345678;
    return (*(u8 *)&data) == 0x78;
}

bool is_valid(elf_header_64 *elf)
{
    if (elf->ident[0] == 0x7f)
    {
        if (elf->ident[1] == 'E' && elf->ident[2] == 'L' && elf->ident[3] == 'F')
        {
            if (elf->ident[4] == 2 && elf->ident[5] == ((u8)!is_little_endian() + 1))
            {
                if (elf->ident[6] == 1)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

program_64 *load_segment(elf_header_64 *elf_header, fs::vfs::file *file)
{
    // less than 64Kib
    u64 len = elf_header->phnum * sizeof(program_64);
    u64 start = elf_header->phoff;
    byte *p = (byte *)memory::KernelMemoryAllocatorV->allocate(len, 0);
    if (!p)
        return (program_64 *)p;
    file->move(start);
    file->read(p, len);
    return (program_64 *)p;
}

bool elf_handle::load(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info)
{
    elf_header_64 *elf = (elf_header_64 *)header;
    if (!is_valid(elf))
        return false;
    if (elf->shentsize != sizeof(section_64) || elf->phentsize != sizeof(program_64))
        return false;

    auto &vma = new_mm_info->vma;
    using namespace memory::vm;
    using namespace arch::task;

    program_64 *program = load_segment(elf, file);
    if (program == nullptr)
        return false;

    program_64 *program_last = program + elf->phnum;

    while (program != program_last)
    {
        if (program->type == program_type::load)
        {
            flag_t flag = 0;
            if (program->flags & 1)
            {
                flag |= flags::executeable;
            }
            if (program->flags & 2)
            {
                flag |= flags::writeable;
            }
            if (program->flags & 4)
            {
                flag |= flags::readable;
            }
            u64 start = program->vaddr & ~(memory::page_size - 1);
            //  u64 end = start + memory::page_size;
            u64 off = program->offset & ~(memory::page_size - 1);

            memory::vm::map_file(start, new_mm_info, file, off, program->file_size + (program->offset - off), flag);
        }
        else if (program->type == program_type::interp)
        {
        }
        program++;
    }

    // stack mapping, stack size 8MB
    auto stack_vm = vma.allocate_map(memory::user_stack_maximum_size,
                                     memory::vm::flags::readable | memory::vm::flags::writeable |
                                         memory::vm::flags::expand | memory::vm::flags::user_mode,
                                     memory::vm::fill_expand_vm, 0);

    info->stack_top = (void *)stack_vm->end;
    info->stack_bottom = (void *)stack_vm->start;
    info->entry_start_address = (void *)elf->entry;
    return true;
}

} // namespace bin_handle
