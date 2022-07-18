#include "kernel/task/binary_handle/elf.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/task.hpp"
#include "kernel/cpu.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/memory.hpp"
#include "kernel/util/str.hpp"
#include <cstdint>
#include <type_traits>

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
    // The array element is unused; other members' values are undefined. This type lets the program header table have
    // ignored entries.
    null,
    // The array element specifies a loadable segment, described by p_filesz and p_memsz. The bytes from the file are
    // mapped to the beginning of the memory segment. If the segment's memory size (p_memsz) is larger than the file
    // size (p_filesz), the ``extra'' bytes are defined to hold the value 0 and to follow the segment's initialized
    // area. The file size may not be larger than the memory size. Loadable segment entries in the program header table
    // appear in ascending order, sorted on the p_vaddr member.
    load,
    // The array element specifies dynamic linking information.
    dynamic,
    // The array element specifies the location and size of a null-terminated path name to invoke as an interpreter.
    // This segment type is meaningful only for executable files (though it may occur for shared objects); it may not
    // occur more than once in a file. If it is present, it must precede any loadable segment entry.
    interp,
    // The array element specifies the location and size of auxiliary information.
    note,
    // This segment type is reserved but has unspecified semantics. Programs that contain an array element of this type
    // do not conform to the ABI.
    shlib,
    // The array element, if present, specifies the location and size of the program header table itself, both in the
    // file and in the memory image of the program. This segment type may not occur more than once in a file. Moreover,
    // it may occur only if the program header table is part of the memory image of the program. If it is present, it
    // must precede any loadable segment entry.
    phdr,
    // The array element specifies the Thread-Local Storage template. Implementations need not support this program
    // table entry.
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
    // This member tells what kind of segment this array element describes or how to interpret the array element's
    // information
    u32 type;
    // This member gives flags relevant to the segment.
    u32 flags;
    // This member gives the offset from the beginning of the file at which the first byte of the segment resides.
    u64 offset;
    // This member gives the virtual address at which the first byte of the segment resides in memory.
    u64 vaddr;
    // On systems for which physical addressing is relevant, this member is reserved for the segment's physical address.
    // Because System V ignores physical addressing for application programs, this member has unspecified contents for
    // executable files and shared objects.
    u64 paddr;
    // This member gives the number of bytes in the file image of the segment; it may be zero.
    u64 file_size;
    // This member gives the number of bytes in the memory image of the segment; it may be zero.
    u64 mm_size;
    // As ``Program Loading'' describes in this chapter of the processor supplement, loadable process segments must have
    // congruent values for p_vaddr and p_offset, modulo the page size. This member gives the value to which the
    // segments are aligned in memory and in the file. Values 0 and 1 mean no alignment is required. Otherwise, p_align
    // should be a positive, integral power of 2, and p_vaddr should equal p_offset, modulo p_align.
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
    byte *p = (byte *)memory::MemoryAllocatorV->allocate(len, alignof(program_64));
    if (!p)
        return (program_64 *)p;
    file->move(start);
    file->read(p, len, 0);
    return reinterpret_cast<program_64 *>(p);
}

/// TODO: release memory resource
bool elf_handle::load(byte *header, fs::vfs::file *file, memory::vm::info_t *new_mm_info, execute_info *info)
{
    elf_header_64 *elf = reinterpret_cast<elf_header_64 *>(header);
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

    u64 loaded_max_address = 0;

    while (program != program_last)
    {
        if (program->type == program_type::load)
        {
            flag_t flag = flags::user_mode;
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
            u64 align_offset = program->vaddr - start;
            // trace::info("va ", trace::hex(program->vaddr), " start ", trace::hex(start), " off ",
            //             trace::hex(program->offset), " file_size ", trace::hex(program->file_size), " mmsize ",
            //             trace::hex(program->mm_size), " ba ", trace::hex(align_offset));

            if (program->offset < align_offset)
            {
                trace::warning("program offset ", trace::hex(program->offset), " align offset ",
                               trace::hex(align_offset));
                return false;
            }

            u64 offset = program->offset - align_offset;
            u64 mm_size = program->mm_size + align_offset;
            u64 fsize = program->file_size + align_offset;
            u64 end = start + mm_size;
            if (loaded_max_address < end)
                loaded_max_address = end;
            new_mm_info->map_file(start, file, offset, fsize, mm_size, flag);
        }
        else if (program->type == program_type::interp)
        {
        }
        program++;
    }
    if (loaded_max_address == 0)
    {
        trace::warning("loaded max address is zero");
        return false;
    }
    u64 brk_beg = ((loaded_max_address + memory::page_size - 1) & ~(memory::page_size - 1));

    if (!new_mm_info->init_brk(brk_beg))
    {
        trace::warning("alloc brk fail");
        return false;
    }

    // stack mapping, stack size 8MB
    auto stack_vm = vma.allocate_map(memory::user_stack_maximum_size,
                                     memory::vm::flags::readable | memory::vm::flags::writeable |
                                         memory::vm::flags::expand | memory::vm::flags::user_mode,
                                     memory::vm::fill_expand_vm, 0);
    if (stack_vm == nullptr)
    {
        trace::warning("empty start_vm");
        return false;
    }
    // trace::info("max load ", trace::hex(loaded_max_address), " brk ", trace::hex(brk_beg), " stack ",
    //             trace::hex(stack_vm->end), "-", trace::hex(stack_vm->start));

    info->stack_top = (void *)stack_vm->end;
    info->stack_bottom = (void *)stack_vm->start;
    info->entry_start_address = (void *)elf->entry;

    return true;
}

} // namespace bin_handle
