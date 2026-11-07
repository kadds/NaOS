#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/tss_bitmap.hpp"
#include "kernel/task/io_lease_lifecycle.hpp"

TEST_CASE("deny-all TSS mapping is fail-closed without clearing a stale bitmap", "[tss]")
{
    arch::tss::tss_storage storage{};
    storage.io_bitmap[0] = 0;
    storage.tss.io_address = sizeof(arch::tss::tss_t);

    arch::tss::detail::deny_all(storage);

    CHECK(storage.tss.io_address == arch::tss::detail::denied_io_map_base);
    CHECK(storage.tss.io_address > arch::tss::detail::io_bitmap_descriptor_limit);
    CHECK(storage.io_bitmap[0] == 0);
    CHECK(storage.io_bitmap[arch::tss::io_bitmap_bytes] == 0xff);
}
TEST_CASE("granting a TSS range rebuilds the bitmap and keeps the guard byte", "[tss]")
{
    arch::tss::tss_storage storage{};
    storage.io_bitmap[0] = 0;
    storage.io_bitmap[1] = 0;
    const arch::tss::io_range range{8, 1};

    arch::tss::detail::allow_ranges(storage, &range, 1);

    CHECK(storage.tss.io_address == sizeof(arch::tss::tss_t));
    CHECK(storage.io_bitmap[0] == 0xff);
    CHECK(storage.io_bitmap[1] == 0xfe);
    CHECK(storage.io_bitmap[arch::tss::io_bitmap_bytes] == 0xff);
}

TEST_CASE("TSS bitmap cache skips identical leases and rebuilds changed lease sets", "[tss]")
{
    arch::tss::tss_storage storage{};
    const arch::tss::io_range first{8, 1};
    const arch::tss::io_range changed{16, 1};

    arch::tss::detail::allow_ranges(storage, &first, 1);
    storage.io_bitmap[0] = 0;
    arch::tss::detail::allow_ranges(storage, &first, 1);
    CHECK(storage.io_bitmap[0] == 0);

    arch::tss::detail::allow_ranges(storage, &changed, 1);
    CHECK(storage.io_bitmap[0] == 0xff);
    CHECK(storage.io_bitmap[2] == 0xfe);
}

TEST_CASE("exec revocation clears thread I/O leases, advances generation, and denies the CPU bitmap", "[tss]")
{
    arch::tss::tss_storage storage{};
    arch::tss::io_range ranges[2]{{0x3f8, 8}, {0x60, 4}};
    u32 range_count = 2;
    constexpr u64 old_generation = 41;
    arch::tss::detail::allow_ranges(storage, ranges, range_count);
    CHECK((storage.io_bitmap[0x3f8 / 8] & (1u << (0x3f8 % 8))) == 0);
    CHECK((storage.io_bitmap[0x60 / 8] & (1u << (0x60 % 8))) == 0);

    const auto next_generation = task::detail::revoke_direct_io_lease_state(
        ranges, range_count, old_generation, [&] { arch::tss::detail::deny_all(storage); });

    CHECK(range_count == 0);
    CHECK(ranges[0].port_count == 0);
    CHECK(ranges[1].port_count == 0);
    CHECK(next_generation == old_generation + 1);
    CHECK_FALSE(task::detail::io_lease_generation_is_current(next_generation, old_generation));
    CHECK(storage.tss.io_address > arch::tss::detail::io_bitmap_descriptor_limit);
}

TEST_CASE("I/O lease generation fails closed instead of wrapping", "[tss]")
{
    u32 range_count = 0;
    struct empty_range
    {
        u16 first_port = 0;
        u32 port_count = 0;
    } ranges[1]{};

    const auto exhausted_generation = task::detail::revoke_direct_io_lease_state(ranges, range_count, ~u64(0), [] {});

    CHECK(exhausted_generation == 0);
    CHECK_FALSE(task::detail::io_lease_generation_is_current(exhausted_generation, exhausted_generation));
}
