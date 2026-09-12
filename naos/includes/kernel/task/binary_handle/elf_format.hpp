#pragma once

#include "freelibcxx/vector.hpp"
#include <cstdint>

/// ELF64 format constants, header validation and PT_LOAD range computation.
/// This header is deliberately free of kernel-only dependencies so the loader
/// admission logic stays unit-testable on the host (USERSPACE_FILESYSTEM_ADR
/// Phase 1 exit criteria).
namespace bin_handle::elf_format
{

#define NA_ELF_PACKED __attribute__((packed))

struct elf_header_64
{
    unsigned char ident[16];
    /// elf_type
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    /// execute the entry point
    uint64_t entry;
    /// program header table offset
    uint64_t phoff;
    /// section header table offset
    uint64_t shoff;
    /// eflags
    uint32_t flags;
    /// header size. For 64-bit is 64
    uint16_t ehsize;
    /// the size of each entry in the program header table.
    uint16_t phentsize;
    uint16_t phnum;
    ///  a section header's size in bytes
    uint16_t shentsize;
    uint16_t shnum;
    /// section header string table index.
    uint16_t shstrndx;
} NA_ELF_PACKED;

struct section_64
{
    uint32_t name_index;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t align;
    uint64_t entsize;
} NA_ELF_PACKED;

struct program_64
{
    /// Segment type (see program_type).
    uint32_t type;
    /// Segment flags: bit0 execute, bit1 write, bit2 read.
    uint32_t flags;
    /// File offset of the segment.
    uint64_t offset;
    /// Virtual address of the segment.
    uint64_t vaddr;
    /// Reserved physical address; unspecified for application programs.
    uint64_t paddr;
    /// Byte count of the segment in the file image.
    uint64_t file_size;
    /// Byte count of the segment in memory; BSS extends beyond file_size.
    uint64_t mm_size;
    /// Alignment of the segment.
    uint64_t align;
} NA_ELF_PACKED;

namespace program_type
{
enum : uint32_t
{
    null = 0,
    load = 1,
    dynamic = 2,
    interp = 3,
    note = 4,
    shlib = 5,
    phdr = 6,
    tls = 7,
    gnu_eh_frame = 0x6474e550,
    gnu_stack,
    gnu_relro,
};
} // namespace program_type

/// Failure reasons reported by the admission helpers. Callers translate them
/// into diagnostics; the helpers themselves stay side-effect free.
enum class status
{
    ok = 0,
    bad_ident,
    bad_entry_sizes,
    bad_program_table,
    bad_segment_offset,
    segment_out_of_range,
    bad_segment_order,
};

inline bool is_little_endian()
{
    const uint32_t data = 0x12345678;
    return (*reinterpret_cast<const uint8_t *>(&data)) == 0x78;
}

inline bool is_valid(const elf_header_64 &elf)
{
    if (elf.ident[0] != 0x7f)
        return false;
    if (elf.ident[1] != 'E' || elf.ident[2] != 'L' || elf.ident[3] != 'F')
        return false;
    if (elf.ident[4] != 2)
        return false;
    if (elf.ident[5] != static_cast<uint8_t>(!is_little_endian() + 1))
        return false;
    return elf.ident[6] == 1;
}

inline bool valid_entry_sizes(const elf_header_64 &elf)
{
    return elf.shentsize == sizeof(section_64) && elf.phentsize == sizeof(program_64);
}

/// One contiguous PT_LOAD mapping request. data_length bytes starting at
/// object_offset back the range [start, start + map_length); anything beyond
/// data_length is anonymous zero-fill (ELF BSS).
struct segment_range
{
    uint64_t start;         ///< page-aligned virtual start
    uint64_t end;           ///< page-aligned virtual end (covers mem_size)
    uint64_t object_offset; ///< image offset backing 'start'
    uint64_t data_length;   ///< bytes taken from the image
    uint32_t p_flags;       ///< raw ELF p_flags (X=1, W=2, R=4)

    uint64_t map_length() const { return end - start; }

    void move_start(int64_t offset)
    {
        start += offset;
        object_offset += offset;
    }

    void move_end(int64_t offset) { end += offset; }
};

using range_vector_t = freelibcxx::vector<segment_range>;

/// Collect every PT_LOAD entry of the program header table into page-aligned
/// mapping requests, then coalesce adjacent/overlapping requests the same way
/// the kernel file loader always did. On success ranges is filled and
/// loaded_max_address holds the highest mapped byte.
///
/// When the program header table lives inside a loaded segment,
/// *program_header_vaddr receives its in-memory address and
/// *program_header_found is set; callers must initialize the flag to false
/// and may pass null pointers to skip the lookup.
///
/// address_limit bounds both virtual starts and image offsets (the kernel
/// passes its user-space top address); page_size must be a power of two.
inline status build_load_ranges(const elf_header_64 &header, const program_64 *programs, range_vector_t &ranges,
                                uint64_t &loaded_max_address, uint64_t *program_header_vaddr,
                                bool *program_header_found, uint64_t page_size, uint64_t address_limit)
{
    const auto roundup = [page_size](uint64_t value) { return (value + page_size - 1) & ~(page_size - 1); };

    const uint64_t program_header_size = static_cast<uint64_t>(header.phentsize) * header.phnum;
    const uint64_t program_header_end = header.phoff + program_header_size;
    if (program_header_end < header.phoff)
        return status::bad_program_table;

    for (uint16_t index = 0; index < header.phnum; index++)
    {
        const program_64 &entry = programs[index];
        if (entry.type != program_type::load)
            continue;

        // Overflowing file extents reject the whole image.
        const uint64_t program_file_end = entry.offset + entry.file_size;
        if (program_file_end < entry.offset)
            return status::bad_program_table;
        if (program_header_vaddr != nullptr && program_header_found != nullptr && !*program_header_found &&
            header.phoff >= entry.offset && program_header_end <= program_file_end)
        {
            *program_header_vaddr = entry.vaddr + (header.phoff - entry.offset);
            *program_header_found = true;
        }

        const uint64_t start = entry.vaddr & ~(page_size - 1);
        const uint64_t align_offset = entry.vaddr - start;
        if (entry.offset < align_offset)
            return status::bad_segment_offset;

        const uint64_t object_offset = entry.offset - align_offset;
        if (start >= address_limit || object_offset >= address_limit)
            return status::segment_out_of_range;

        const uint64_t memory_size = entry.mm_size + align_offset;
        const uint64_t data_length = entry.file_size + align_offset;
        if (data_length < align_offset || memory_size < data_length)
            return status::bad_segment_offset;
        const uint64_t end = roundup(start + memory_size);
        if (end < start)
            return status::segment_out_of_range;
        if (loaded_max_address < end)
            loaded_max_address = end;

        ranges.push_back(segment_range{start, end, object_offset, data_length, entry.flags});
    }

    // Coalesce neighbouring mappings whose page windows touch. Subset checks
    // on p_flags are invariant under the later bit-permuting conversion to VM
    // flags, so this runs on the raw ELF rights.
    for (int64_t i = 0; ranges.size() > 0 && i < static_cast<int64_t>(ranges.size()) - 1; i++)
    {
        segment_range &cur = ranges[i];
        segment_range &next = ranges[i + 1];
        if (cur.end <= next.start)
            continue;
        const int64_t oversize = static_cast<int64_t>(cur.end - next.start);
        if (cur.start > next.start || cur.end > next.end)
            return status::bad_segment_order;
        if (static_cast<int64_t>(cur.start) - static_cast<int64_t>(cur.object_offset) !=
            static_cast<int64_t>(next.start) - static_cast<int64_t>(next.object_offset))
            return status::bad_segment_order;
        if (cur.p_flags & ~next.p_flags)
        {
            if (next.p_flags & ~cur.p_flags)
            {
                // Split into three pieces so each range keeps its own rights.
                const segment_range middle{next.start, cur.end, next.object_offset,
                                           static_cast<uint64_t>(oversize), cur.p_flags | next.p_flags};
                cur.move_end(-oversize);
                cur.data_length = cur.data_length > static_cast<uint64_t>(oversize)
                                      ? cur.data_length - static_cast<uint64_t>(oversize)
                                      : cur.map_length();

                next.move_start(oversize);
                next.data_length -= oversize;

                ranges.insert_at(i + 1, middle);
                i++;
            }
            else
            {
                next.move_start(oversize);
                cur.data_length -= oversize;
            }
        }
        else
        {
            if (next.p_flags & ~cur.p_flags)
            {
                cur.move_end(-oversize);
                cur.data_length = cur.data_length > static_cast<uint64_t>(oversize)
                                      ? cur.data_length - static_cast<uint64_t>(oversize)
                                      : cur.map_length();
            }
            else
            {
                // Safe to merge: same backing window and no extra rights.
                cur.move_end(static_cast<int64_t>(next.end - cur.end));
                cur.data_length = next.object_offset + next.data_length - cur.object_offset;
                ranges.remove_at(i + 1);
            }
        }
    }

    return status::ok;
}

#undef NA_ELF_PACKED

} // namespace bin_handle::elf_format
