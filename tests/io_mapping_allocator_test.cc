#include <catch2/catch_test_macros.hpp>

#include "kernel/mm/io_mapping_allocator.hpp"

#include <atomic>
#include <limits>

TEST_CASE("I/O mapping cursor reservation handles an alignment gap", "[memory][io_mapping]")
{
    std::atomic_uint64_t cursor{0x1003};

    CHECK(memory::detail::reserve_io_mapping_address(cursor, 0x1000, 0x2000) == 0x2000);
    CHECK(cursor.load() == 0x3000);
    CHECK(memory::detail::reserve_io_mapping_address(cursor, 0x1000, 0x1000) == 0x3000);
    CHECK(cursor.load() == 0x4000);
}

TEST_CASE("I/O mapping cursor rejects invalid alignment and overflow", "[memory][io_mapping]")
{
    std::atomic_uint64_t cursor{0x1000};
    CHECK(memory::detail::reserve_io_mapping_address(cursor, 0, 0x1000) == 0);
    CHECK(memory::detail::reserve_io_mapping_address(cursor, 0x1000, 0) == 0);
    CHECK(memory::detail::reserve_io_mapping_address(cursor, 0x1000, 0x1800) == 0);

    cursor.store(std::numeric_limits<u64>::max() - 0x100);
    CHECK(memory::detail::reserve_io_mapping_address(cursor, 0x200, 0x1000) == 0);
}
