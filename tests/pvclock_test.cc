#include "kernel/arch/kvm_pvclock.hpp"
#include "kernel/time/event_source.hpp"

#include "catch2_compat.hpp"
#include <cstdint>
#include <limits>

namespace
{
using arch::kvm_pvclock::cpuid_snapshot;
using arch::kvm_pvclock::pvclock_sample;

cpuid_snapshot kvm_snapshot(u32 feature_bits)
{
    cpuid_snapshot result{};
    result.hypervisor_present = true;
    result.max_leaf = arch::kvm_pvclock::kvm_clock_feature_leaf;
    result.vendor_ebx = 0x4B4D564B; // KVMK
    result.vendor_ecx = 0x564B4D56; // VMKV
    result.vendor_edx = 0x0000004D; // M + NUL padding
    result.feature_eax = feature_bits;
    return result;
}

void test_kvm_probe_matrix()
{
    REQUIRE(arch::kvm_pvclock::probe(kvm_snapshot(1u << arch::kvm_pvclock::clocksource2_bit)));
    REQUIRE(arch::kvm_pvclock::probe(kvm_snapshot(1u << arch::kvm_pvclock::clocksource2_bit)).cross_cpu_monotonic ==
            false);
    REQUIRE(arch::kvm_pvclock::probe(
                kvm_snapshot((1u << arch::kvm_pvclock::clocksource2_bit) | (1u << arch::kvm_pvclock::stable_bit)))
                .cross_cpu_monotonic);

    auto no_hypervisor = kvm_snapshot(1u << arch::kvm_pvclock::clocksource2_bit);
    no_hypervisor.hypervisor_present = false;
    REQUIRE(!arch::kvm_pvclock::probe(no_hypervisor).available);

    auto wrong_vendor = kvm_snapshot(1u << arch::kvm_pvclock::clocksource2_bit);
    wrong_vendor.vendor_ebx = 0;
    REQUIRE(!arch::kvm_pvclock::probe(wrong_vendor).available);

    auto old_leaf = kvm_snapshot(1u << arch::kvm_pvclock::clocksource2_bit);
    old_leaf.max_leaf = arch::kvm_pvclock::kvm_clock_feature_leaf - 1;
    REQUIRE(!arch::kvm_pvclock::probe(old_leaf).available);
}

void test_policy_matrix()
{
    REQUIRE(arch::kvm_pvclock::parse_policy("auto") == arch::kvm_pvclock::policy::auto_select);
    REQUIRE(arch::kvm_pvclock::parse_policy("on") == arch::kvm_pvclock::policy::required);
    REQUIRE(arch::kvm_pvclock::parse_policy("off") == arch::kvm_pvclock::policy::disabled);
    REQUIRE(arch::kvm_pvclock::parse_policy("unknown") == arch::kvm_pvclock::policy::auto_select);
}

void test_pvclock_fixed_point_conversion()
{
    pvclock_sample sample{};
    sample.version = 2;
    sample.tsc_timestamp = 100;
    sample.system_time = 7'000;
    sample.tsc_to_system_mul = 1u << 31;
    sample.tsc_shift = 0;
    u64 now = 0;

    REQUIRE(arch::kvm_pvclock::try_compute_time(sample, 300, now));
    REQUIRE(now == 7'100);

    sample.tsc_shift = 1;
    REQUIRE(arch::kvm_pvclock::try_compute_time(sample, 300, now));
    REQUIRE(now == 7'200);

    sample.tsc_shift = -1;
    REQUIRE(arch::kvm_pvclock::try_compute_time(sample, 300, now));
    REQUIRE(now == 7'050);

    sample.tsc_shift = 64;
    REQUIRE(!arch::kvm_pvclock::try_compute_time(sample, 300, now));

    sample.tsc_shift = 0;
    sample.system_time = std::numeric_limits<u64>::max();
    REQUIRE(!arch::kvm_pvclock::try_compute_time(sample, 300, now));
}

void test_pvclock_sample_validation()
{
    pvclock_sample sample{};
    sample.version = 2;
    sample.tsc_to_system_mul = 1;
    REQUIRE(arch::kvm_pvclock::valid_sample(sample));
    sample.tsc_shift = -64;
    REQUIRE(!arch::kvm_pvclock::valid_sample(sample));
    sample.tsc_shift = 0;
    sample.tsc_to_system_mul = 0;
    REQUIRE(!arch::kvm_pvclock::valid_sample(sample));
}

void test_lapic_deadline_conversion()
{
    const auto one_tick = timeclock::event_source::convert_deadline(1, 100'000'000, 0xFFFF'FFFF);
    REQUIRE(one_tick.ticks == 1);
    REQUIRE(!one_tick.saturated);

    const auto rounded_up = timeclock::event_source::convert_deadline(11, 100'000'000, 0xFFFF'FFFF);
    REQUIRE(rounded_up.ticks == 2);

    const auto long_deadline =
        timeclock::event_source::convert_deadline(std::numeric_limits<u64>::max(), 100'000'000, 0xFFFF'FFFF);
    REQUIRE(long_deadline.ticks == 0xFFFF'FFFF);
    REQUIRE(long_deadline.saturated);

    const auto remainder_overflow = timeclock::event_source::convert_deadline(999'999'999, 5'000'000'000, 0xFFFF'FFFF);
    REQUIRE(remainder_overflow.ticks == 0xFFFF'FFFF);
    REQUIRE(remainder_overflow.saturated);
}

void test_freestanding_unsigned_math()
{
    u64 result = 0;
    REQUIRE(timeclock::unsigned_math::try_mul_div_floor(1'000'000'000, 1'000'000'000, 1'000'000'000, result));
    REQUIRE(result == 1'000'000'000);
    REQUIRE(timeclock::unsigned_math::try_mul_div_ceil(10, 1, 3, result));
    REQUIRE(result == 4);
    REQUIRE(timeclock::unsigned_math::try_mul_div_floor(
        std::numeric_limits<u64>::max(), std::numeric_limits<u64>::max(), std::numeric_limits<u64>::max(), result));
    const auto product =
        timeclock::unsigned_math::multiply(std::numeric_limits<u64>::max(), std::numeric_limits<u64>::max());
    REQUIRE(product.low == 1);
    REQUIRE(product.high == std::numeric_limits<u64>::max() - 1);
    REQUIRE(result == std::numeric_limits<u64>::max());
}

void test_event_source_contract()
{
    const timeclock::deadline_request request{100, 200, 7};
    REQUIRE(request.is_valid());
    REQUIRE(request.delta_ns() == 100);
    REQUIRE(!timeclock::deadline_request{200, 100, 7}.is_valid());
    REQUIRE(timeclock::deadline_request{200, 100, 7}.delta_ns() == 0);
    REQUIRE(!timeclock::deadline_request{100, 200, 0}.is_valid());
    REQUIRE(timeclock::arm_result::armed != timeclock::arm_result::invalid);
    REQUIRE(timeclock::arm_result::armed != timeclock::arm_result::unavailable);
}
} // namespace

TEST_CASE("KVM pvclock and event source contracts", "[pvclock][timer]")
{
    test_kvm_probe_matrix();
    test_policy_matrix();
    test_pvclock_fixed_point_conversion();
    test_pvclock_sample_validation();
    test_lapic_deadline_conversion();
    test_freestanding_unsigned_math();
    test_event_source_contract();
}
