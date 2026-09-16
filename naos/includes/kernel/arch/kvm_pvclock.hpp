#pragma once

#include "kernel/time/event_clock.hpp"
#include "kernel/types.hpp"
#include <atomic>

namespace arch::kvm_pvclock
{

constexpr u32 kvm_clock_vendor_leaf = 0x40000000;
constexpr u32 kvm_clock_feature_leaf = 0x40000001;
constexpr u32 clocksource2_bit = 3;
constexpr u32 stable_bit = 24;
constexpr u32 msr_kvm_system_time_new = 0x4B564D01;
constexpr u32 max_sample_retries = 8;

struct cpuid_snapshot
{
    bool hypervisor_present = false;
    u32 max_leaf = 0;
    u32 vendor_ebx = 0;
    u32 vendor_ecx = 0;
    u32 vendor_edx = 0;
    u32 feature_eax = 0;
};

struct probe_result
{
    bool available = false;
    bool cross_cpu_monotonic = false;
    bool clocksource2 = false;
    bool stable_bit = false;

    constexpr explicit operator bool() const noexcept { return available; }
};

constexpr probe_result probe(const cpuid_snapshot &snapshot) noexcept
{
    constexpr u32 vendor_ebx = 0x4B4D564B; // "KVMK"
    constexpr u32 vendor_ecx = 0x564B4D56; // "VMKV"
    constexpr u32 vendor_edx = 0x0000004D; // "M" followed by NUL padding
    const bool vendor_matches =
        snapshot.vendor_ebx == vendor_ebx && snapshot.vendor_ecx == vendor_ecx && snapshot.vendor_edx == vendor_edx;
    const bool clocksource2 = (snapshot.feature_eax & (1u << clocksource2_bit)) != 0;
    const bool stable = (snapshot.feature_eax & (1u << stable_bit)) != 0;
    return {snapshot.hypervisor_present && snapshot.max_leaf >= kvm_clock_feature_leaf && vendor_matches &&
                clocksource2,
            snapshot.hypervisor_present && snapshot.max_leaf >= kvm_clock_feature_leaf && vendor_matches &&
                clocksource2 && stable,
            clocksource2, stable};
}

enum class policy : u8
{
    auto_select,
    required,
    disabled,
};

constexpr bool string_equals(const char *value, const char *expected) noexcept
{
    if (value == nullptr || expected == nullptr)
        return false;
    while (*value != 0 || *expected != 0)
    {
        if (*value++ != *expected++)
            return false;
    }
    return true;
}

constexpr policy parse_policy(const char *value) noexcept
{
    if (string_equals(value, "on"))
        return policy::required;
    if (string_equals(value, "off"))
        return policy::disabled;
    return policy::auto_select;
}

/// Exact KVM pvclock ABI.  KVM owns all fields in this page.
struct __attribute__((packed)) pvclock_vcpu_time_info
{
    u32 version;
    u32 pad0;
    u64 tsc_timestamp;
    u64 system_time;
    u32 tsc_to_system_mul;
    i8 tsc_shift;
    u8 flags;
    u8 pad[2];
};
static_assert(sizeof(pvclock_vcpu_time_info) == 32);

struct pvclock_sample
{
    u32 version = 0;
    u64 tsc_timestamp = 0;
    u64 system_time = 0;
    u32 tsc_to_system_mul = 0;
    i8 tsc_shift = 0;
    u8 flags = 0;
};

constexpr bool valid_sample(const pvclock_sample &sample) noexcept
{
    return sample.version != 0 && (sample.version & 1u) == 0 && sample.tsc_to_system_mul != 0 &&
           sample.tsc_shift >= -63 && sample.tsc_shift <= 63;
}

struct wide_u96
{
    u64 low = 0;
    u32 high = 0;
};

/// Exact 64x32 multiplication.  The result is represented as a 96-bit value
/// so the fixed-point conversion never silently truncates its intermediate.
constexpr wide_u96 multiply_u64_u32(u64 value, u32 multiplier) noexcept
{
    const u64 low_limb = static_cast<u32>(value);
    const u64 high_limb = value >> 32;
    const u64 low_product = low_limb * multiplier;
    const u64 high_product = high_limb * multiplier;
    const u64 middle = (high_product & 0xFFFF'FFFFULL) + (low_product >> 32);
    const u32 middle_limb = static_cast<u32>(middle);
    const u32 high = static_cast<u32>((high_product >> 32) + (middle >> 32));
    return {static_cast<u64>(static_cast<u32>(low_product)) | (static_cast<u64>(middle_limb) << 32), high};
}

constexpr bool product_bit(const wide_u96 &product, u32 bit) noexcept
{
    if (bit < 64)
        return ((product.low >> bit) & 1u) != 0;
    return ((product.high >> (bit - 64)) & 1u) != 0;
}

constexpr bool try_shift_product_to_u64(const wide_u96 &product, i32 right_shift, u64 &result) noexcept
{
    if (right_shift >= 0)
    {
        const u32 shift = static_cast<u32>(right_shift);
        if (shift >= 96)
        {
            result = 0;
            return true;
        }
        for (u32 bit = shift + 64; bit < 96; bit++)
        {
            if (product_bit(product, bit))
                return false;
        }
        result = 0;
        for (u32 bit = 0; bit < 64 && bit + shift < 96; bit++)
        {
            if (product_bit(product, bit + shift))
                result |= 1ULL << bit;
        }
        return true;
    }

    const u32 left_shift = static_cast<u32>(-right_shift);
    if (left_shift >= 64)
    {
        result = 0;
        return product.low == 0 && product.high == 0;
    }
    for (u32 bit = 64 - left_shift; bit < 96; bit++)
    {
        if (product_bit(product, bit))
            return false;
    }
    result = 0;
    for (u32 bit = left_shift; bit < 64; bit++)
    {
        if (product_bit(product, bit - left_shift))
            result |= 1ULL << bit;
    }
    return true;
}

constexpr bool try_compute_elapsed(u64 delta_tsc, u32 multiplier, i8 shift, u64 &elapsed_ns) noexcept
{
    if (shift < -63 || shift > 63 || multiplier == 0)
        return false;
    const auto product = multiply_u64_u32(delta_tsc, multiplier);
    const i32 right_shift = 32 - static_cast<i32>(shift);
    return try_shift_product_to_u64(product, right_shift, elapsed_ns);
}

constexpr bool try_compute_time(const pvclock_sample &sample, u64 current_tsc, u64 &now_ns) noexcept
{
    if (!valid_sample(sample) || current_tsc < sample.tsc_timestamp)
        return false;
    u64 elapsed_ns = 0;
    if (!try_compute_elapsed(current_tsc - sample.tsc_timestamp, sample.tsc_to_system_mul, sample.tsc_shift,
                             elapsed_ns))
        return false;
    if (elapsed_ns > static_cast<u64>(-1) - sample.system_time)
        return false;
    now_ns = sample.system_time + elapsed_ns;
    return true;
}

struct health_counters
{
    u64 stable_samples = 0;
    u64 seqlock_retries = 0;
    u64 cached_fallbacks = 0;
    u64 backward_clamps = 0;
    u64 invalid_samples = 0;
    u64 paused_samples = 0;
};

class clock final : public timeclock::event_clock
{
  public:
    explicit clock(policy selection = policy::auto_select) noexcept
        : selection_(selection)
    {
    }

    ~clock() override { stop_cpu(); }

    bool start_cpu() noexcept override;
    void stop_cpu() noexcept override;
    timeclock::nanosecond_t now_ns() noexcept override;
    const char *name() const noexcept override { return "kvm-pvclock"; }
    bool cross_cpu_monotonic() const noexcept override { return cross_cpu_monotonic_; }

    static bool read_sample(const volatile pvclock_vcpu_time_info *info, pvclock_sample &sample, u32 &retries,
                            u64 *sample_tsc = nullptr) noexcept;
    health_counters health() const noexcept;
    bool take_retry_warning() noexcept;
    bool started() const noexcept { return page_ != nullptr; }

  private:
    policy selection_;
    void *page_ = nullptr;
    volatile pvclock_vcpu_time_info *info_ = nullptr;
    pvclock_sample cached_sample_{};
    u64 last_returned_ns_ = 0;
    bool cross_cpu_monotonic_ = false;
    std::atomic_uint64_t stable_samples_{0};
    std::atomic_uint64_t seqlock_retries_{0};
    std::atomic_uint64_t cached_fallbacks_{0};
    std::atomic_uint64_t backward_clamps_{0};
    std::atomic_uint64_t invalid_samples_{0};
    std::atomic_uint64_t paused_samples_{0};
    std::atomic_uint64_t retry_streak_{0};
    std::atomic_bool retry_warning_pending_{false};
};

clock *make_event_clock(policy selection = policy::auto_select) noexcept;
probe_result detect() noexcept;

} // namespace arch::kvm_pvclock
