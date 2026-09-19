#include "kernel/task/binary_handle/elf.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/arch/mm.hpp"
#include "kernel/arch/paging.hpp"
#include "kernel/arch/task.hpp"
#include "kernel/cpu.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
#include "kernel/task/binary_handle/elf_format.hpp"
#include <cstdint>
#include <utility>

KLOG_MODULE(task);
namespace bin_handle
{
namespace
{

using elf_format::elf_header_64;
using elf_format::program_64;

/// Byte source plus mapping strategy for one executable image.
class exec_image
{
  public:
    virtual ~exec_image() = default;
    /// Read up to count bytes at offset; returns the number of bytes read.
    virtual u64 read(u64 offset, byte *buffer, u64 count) = 0;
    /// Map [start, start + map_length) backed by data_length image bytes.
    virtual const memory::vm::vm_t *map(u64 start, u64 object_offset, u64 data_length, u64 map_length,
                                        flag_t flags) = 0;
};

class object_image final : public exec_image
{
  public:
    object_image(const handle_t<naos::data_plane::memory_object> &object, const khandle &backing,
                 memory::vm::info_t *info)
        : object_(object)
        , backing_(backing)
        , info_(info)
    {
    }

    u64 read(u64 offset, byte *buffer, u64 count) override
    {
        u64 actual = 0;
        return object_->read(offset, buffer, count, actual) == NA_STATUS_OK ? actual : 0;
    }

    const memory::vm::vm_t *map(u64 start, u64 object_offset, u64 data_length, u64 map_length, flag_t flags) override
    {
        // Every mapping holds its own reference on the backing capability so
        // the creator may drop its handle immediately after spawn.
        // Read-only ELF segments can alias the immutable MemoryObject pages;
        // writable segments remain private and are populated through the
        // normal fault-and-copy path.
        if ((flags & memory::vm::flags::writeable) == 0)
            flags |= memory::vm::flags::shared;
        return info_->map_memory_object(start, backing_, &object_, object_offset, 0, data_length, map_length, flags);
    }

  private:
    handle_t<naos::data_plane::memory_object> object_;
    khandle backing_;
    memory::vm::info_t *info_;
};

flag_t paging_flags_of(u32 p_flags)
{
    using namespace memory::vm;
    flag_t flag = flags::user_mode;
    if (p_flags & 1)
        flag |= flags::executeable;
    if (p_flags & 2)
        flag |= flags::writeable;
    if (p_flags & 4)
        flag |= flags::readable;
    return flag;
}

const char *admission_status_name(elf_format::status status)
{
    switch (status)
    {
        case elf_format::status::ok:
            break;
        case elf_format::status::bad_ident:
            return "invalid ident";
        case elf_format::status::bad_entry_sizes:
            return "invalid shentsize/phentsize";
        case elf_format::status::bad_program_table:
            return "invalid program header extent";
        case elf_format::status::bad_segment_offset:
            return "segment offset misaligned";
        case elf_format::status::segment_out_of_range:
            return "segment outside user range";
        case elf_format::status::bad_segment_order:
            return "overlapping segments cannot merge";
    }
    return "unknown";
}

/// Shared ELF loading for every byte source. Returns false on any admission
/// failure; nothing is mapped until the first successful segment mapping.
bool load_common(const byte *header, exec_image &image, memory::vm::info_t *new_mm_info, execute_info *info)
{
    auto &vma = new_mm_info->vma();
    const auto *elf = reinterpret_cast<const elf_header_64 *>(header);

    auto admission = elf_format::status::ok;
    if (!elf_format::is_valid(*elf))
        admission = elf_format::status::bad_ident;
    else if (!elf_format::valid_entry_sizes(*elf))
        admission = elf_format::status::bad_entry_sizes;
    if (admission != elf_format::status::ok)
    {
        KLOG_WARN("invalid ELF {}: {:x} {:x} {:x} {:x}", admission_status_name(admission), elf->ident[0], elf->ident[1],
                  elf->ident[2], elf->ident[3]);
        return false;
    }

    const u64 program_table_bytes = static_cast<u64>(elf->phentsize) * elf->phnum;
    auto *programs =
        static_cast<program_64 *>(memory::MemoryAllocatorV->allocate(program_table_bytes, alignof(program_64)));
    if (programs == nullptr)
        return false;
    if (image.read(elf->phoff, reinterpret_cast<byte *>(programs), program_table_bytes) != program_table_bytes)
    {
        memory::MemoryAllocatorV->deallocate(programs);
        return false;
    }

    elf_format::range_vector_t ranges(memory::KernelCommonAllocatorV);
    ranges.ensure(8);
    u64 loaded_max_address = 0;
    u64 program_header_vaddr = 0;
    bool program_header_found = false;
    const auto range_status =
        elf_format::build_load_ranges(*elf, programs, ranges, loaded_max_address, &program_header_vaddr,
                                      &program_header_found, memory::page_size, memory::user_mmap_top_address);
    if (range_status != elf_format::status::ok)
    {
        KLOG_WARN("ELF program headers rejected: {}", admission_status_name(range_status));
        memory::MemoryAllocatorV->deallocate(programs);
        return false;
    }

    info->program_header = program_header_found ? reinterpret_cast<void *>(program_header_vaddr) : nullptr;
    info->program_header_entry_size = elf->phentsize;
    info->program_header_count = elf->phnum;
    // NaOS currently loads the executable at its linked address and does not
    // load a separate ELF interpreter, so both values are zero for now.
    info->base_address = 0;
    info->hwcap = 0;

    bool mapped_all = true;
    for (const auto &item : ranges)
    {
        if (!mapped_all)
            break;
        auto vm = image.map(item.start, item.object_offset, item.data_length, item.map_length(),
                            paging_flags_of(item.p_flags));
        if (vm == nullptr)
        {
            KLOG_WARN("map {}-{} fail", log::hex(item.start), log::hex(item.end));
            mapped_all = false;
        }
    }
    memory::MemoryAllocatorV->deallocate(programs);
    if (!mapped_all)
        return false;

    if (loaded_max_address == 0)
    {
        KLOG_WARN("loaded max address is zero");
        return false;
    }
    u64 brk_beg = ((loaded_max_address + memory::page_size - 1) & ~(memory::page_size - 1));

    if (!new_mm_info->init_brk(brk_beg))
    {
        KLOG_WARN("alloc brk fail");
        return false;
    }

    // stack mapping, stack size 8MB
    auto stack_vm = vma.allocate_map(memory::user_stack_maximum_size,
                                     memory::vm::flags::readable | memory::vm::flags::writeable |
                                         memory::vm::flags::expand | memory::vm::flags::user_mode |
                                         memory::vm::flags::user_stack,
                                     memory::vm::page_fault_method::common, 0);

    if (stack_vm == nullptr)
    {
        KLOG_WARN("empty start_vm");
        return false;
    }

    info->stack_top = (void *)stack_vm->end;
    info->stack_bottom = (void *)stack_vm->start;
    info->entry_start_address = (void *)elf->entry;

    return true;
}

} // namespace

bool elf_handle::load(byte *header, const handle_t<naos::data_plane::memory_object> &object, const khandle &backing,
                      memory::vm::info_t *new_mm_info, execute_info *info)
{
    object_image image(object, backing, new_mm_info);
    return load_common(header, image, new_mm_info, info);
}

} // namespace bin_handle
