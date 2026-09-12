#include <catch2/catch_test_macros.hpp>

#include "kernel/task/binary_handle/elf_format.hpp"
#include <cstring>
#include <malloc.h>
#include <naos/abi.h>

namespace
{

/// Minimal malloc-backed allocator for freelibcxx containers on the host.
class host_allocator final : public freelibcxx::Allocator
{
  public:
    void *allocate(size_t size, size_t align) noexcept override
    {
        (void)align;
        return ::malloc(size);
    }
    void deallocate(void *ptr) noexcept override { ::free(ptr); }
};

host_allocator allocator;

bin_handle::elf_format::elf_header_64 make_header(uint16_t phnum)
{
    bin_handle::elf_format::elf_header_64 header{};
    header.ident[0] = 0x7f;
    header.ident[1] = 'E';
    header.ident[2] = 'L';
    header.ident[3] = 'F';
    header.ident[4] = 2; // ELFCLASS64
    header.ident[5] = 1; // little endian
    header.ident[6] = 1; // SYSV version
    header.ehsize = sizeof(header);
    header.phentsize = sizeof(bin_handle::elf_format::program_64);
    header.phnum = phnum;
    header.shentsize = sizeof(bin_handle::elf_format::section_64);
    return header;
}

bin_handle::elf_format::program_64 make_load(uint64_t vaddr, uint64_t offset, uint64_t file_size, uint64_t mem_size,
                                             uint32_t flags)
{
    bin_handle::elf_format::program_64 entry{};
    entry.type = bin_handle::elf_format::program_type::load;
    entry.flags = flags;
    entry.vaddr = vaddr;
    entry.offset = offset;
    entry.file_size = file_size;
    entry.mm_size = mem_size;
    entry.align = 0x1000;
    return entry;
}

constexpr uint32_t kRx = 5; // R|X
constexpr uint32_t kW = 2;  // W

} // namespace

TEST_CASE("ELF header validation", "[mobj]")
{
    using namespace bin_handle::elf_format;
    const auto header = make_header(1);
    REQUIRE(is_valid(header));
    REQUIRE(valid_entry_sizes(header));

    auto broken = header;
    SECTION("bad magic")
    {
        broken.ident[0] = 'M';
        REQUIRE_FALSE(is_valid(broken));
    }
    SECTION("bad class")
    {
        broken.ident[4] = 1; // ELFCLASS32
        REQUIRE_FALSE(is_valid(broken));
    }
    SECTION("bad data encoding")
    {
        broken.ident[5] = 3;
        REQUIRE_FALSE(is_valid(broken));
    }
    SECTION("bad version")
    {
        broken.ident[6] = 0;
        REQUIRE_FALSE(is_valid(broken));
    }
    SECTION("bad entry sizes")
    {
        broken.phentsize = 32;
        REQUIRE_FALSE(valid_entry_sizes(broken));
        broken = header;
        broken.shentsize = 32;
        REQUIRE_FALSE(valid_entry_sizes(broken));
    }
}

TEST_CASE("single PT_LOAD with BSS produces zero-fill tail", "[mobj]")
{
    using namespace bin_handle::elf_format;
    const auto header = make_header(1);
    // text at 0x200000: 0x800 bytes of file data, 0x1800 bytes in memory (BSS)
    const program_64 programs[] = {make_load(0x200000, 0x1000, 0x800, 0x1800, kRx)};

    range_vector_t ranges(&allocator);
    uint64_t max_address = 0;
    uint64_t phdr_vaddr = 0;
    bool phdr_found = false;
    const auto status =
        build_load_ranges(header, programs, ranges, max_address, &phdr_vaddr, &phdr_found, 0x1000, 0x800000000000UL);

    REQUIRE(status == status::ok);
    REQUIRE(ranges.size() == 1);
    const auto &range = ranges[0];
    CHECK(range.start == 0x200000);
    CHECK(range.end == 0x202000); // one page short of two pages
    CHECK(range.object_offset == 0x1000);
    CHECK(range.data_length == 0x800);
    CHECK(range.map_length() == 0x2000); // BSS extends past the image data
    CHECK(range.p_flags == kRx);
    CHECK(max_address == 0x202000);
    CHECK_FALSE(phdr_found);
}

TEST_CASE("program header table inside a load segment is located", "[mobj]")
{
    using namespace bin_handle::elf_format;
    auto header = make_header(1);
    const program_64 programs[] = {make_load(0x200000, 0x1000, 0x2000, 0x2000, kRx)};
    header.phoff = 0x1040;

    range_vector_t ranges(&allocator);
    uint64_t max_address = 0;
    uint64_t phdr_vaddr = 0;
    bool phdr_found = false;
    REQUIRE(build_load_ranges(header, programs, ranges, max_address, &phdr_vaddr, &phdr_found, 0x1000,
                              0x800000000000UL) == status::ok);
    REQUIRE(phdr_found);
    CHECK(phdr_vaddr == 0x200040);
}

TEST_CASE("touching segments with extended rights are shrunk", "[mobj]")
{
    using namespace bin_handle::elf_format;
    const auto header = make_header(2);
    // RX text window overlaps an RW data segment whose rights are a superset;
    // the loader keeps two mappings and shrinks the first to its own page.
    const program_64 programs[] = {
        make_load(0x200000, 0x1000, 0x1800, 0x1800, kRx),
        make_load(0x201800, 0x2800, 0x800, 0x800, kRx | kW),
    };

    range_vector_t ranges(&allocator);
    uint64_t max_address = 0;
    const auto status = build_load_ranges(header, programs, ranges, max_address, nullptr, nullptr, 0x1000,
                                          0x800000000000UL);
    REQUIRE(status == status::ok);
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].start == 0x200000);
    CHECK(ranges[0].end == 0x201000);
    CHECK(ranges[0].object_offset == 0x1000);
    CHECK(ranges[0].data_length == 0x800);
    CHECK(ranges[0].p_flags == kRx);
    CHECK(ranges[1].start == 0x201000);
    CHECK(ranges[1].end == 0x202000);
    CHECK(ranges[1].object_offset == 0x2000);
    CHECK((ranges[1].p_flags & 1) != 0); // exec preserved on the shared page
    CHECK(max_address == 0x202000);
}

TEST_CASE("overlapping segments with incomparable rights are split in three", "[mobj]")
{
    using namespace bin_handle::elf_format;
    const auto header = make_header(2);
    // RW segment [0x200000, 0x203000) overlaps an RX segment starting at
    // 0x201000; neither rights set is a subset of the other, so the shared
    // page must carry the union while both tails keep their own rights.
    const program_64 programs[] = {
        make_load(0x200000, 0x1000, 0x2000, 0x2800, kW | 4),
        make_load(0x201000, 0x2000, 0x2800, 0x2800, kRx),
    };

    range_vector_t ranges(&allocator);
    uint64_t max_address = 0;
    const auto status = build_load_ranges(header, programs, ranges, max_address, nullptr, nullptr, 0x1000,
                                          0x800000000000UL);
    REQUIRE(status == status::ok);
    REQUIRE(ranges.size() == 3);
    CHECK(ranges[0].start == 0x200000);
    CHECK(ranges[0].end == 0x201000);
    CHECK(ranges[0].object_offset == 0x1000);
    CHECK(ranges[0].data_length == 0x1000);
    CHECK(ranges[0].p_flags == (kW | 4));
    CHECK(ranges[1].start == 0x201000);
    CHECK(ranges[1].end == 0x203000);
    CHECK(ranges[1].object_offset == 0x2000);
    CHECK(ranges[1].data_length == 0x2000);
    CHECK(((ranges[1].p_flags & kW) != 0 && (ranges[1].p_flags & 1) != 0));
    CHECK(ranges[2].start == 0x203000);
    CHECK(ranges[2].end == 0x204000);
    CHECK(ranges[2].object_offset == 0x4000);
    CHECK(ranges[2].data_length == 0x800);
    CHECK(ranges[2].p_flags == kRx);
    CHECK(max_address == 0x204000);
}

TEST_CASE("misaligned segment offsets are rejected", "[mobj]")
{
    using namespace bin_handle::elf_format;
    const auto header = make_header(1);
    // offset 0x100 is smaller than the vaddr page displacement 0x800.
    const program_64 programs[] = {make_load(0x200800, 0x100, 0x800, 0x800, kRx)};

    range_vector_t ranges(&allocator);
    uint64_t max_address = 0;
    REQUIRE(build_load_ranges(header, programs, ranges, max_address, nullptr, nullptr, 0x1000,
                              0x800000000000UL) == status::bad_segment_offset);
}

TEST_CASE("segments beyond the address limit are rejected", "[mobj]")
{
    using namespace bin_handle::elf_format;
    const auto header = make_header(1);
    const program_64 programs[] = {make_load(0x900000000000ULL, 0x1000, 0x800, 0x800, kRx)};

    range_vector_t ranges(&allocator);
    uint64_t max_address = 0;
    REQUIRE(build_load_ranges(header, programs, ranges, max_address, nullptr, nullptr, 0x1000,
                              0x800000000000UL) == status::segment_out_of_range);
}
