#include "kernel/arch/kvm_pvclock.hpp"

#include "kernel/arch/cpu_info.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/common.hpp"
#include "kernel/mm/memory.hpp"

namespace arch::kvm_pvclock
{
namespace
{
constexpr u64 retry_warning_interval = 1'024;

void compiler_barrier() noexcept { __asm__ __volatile__("" : : : "memory"); }

template <typename Atomic> void increment_saturated(Atomic &counter) noexcept
{
    auto old = counter.load(std::memory_order_relaxed);
    while (old != static_cast<u64>(-1) &&
           !counter.compare_exchange_weak(old, old + 1, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

pvclock_sample copy_sample(const volatile pvclock_vcpu_time_info *info) noexcept
{
    pvclock_sample sample;
    sample.version = info->version;
    sample.tsc_timestamp = info->tsc_timestamp;
    sample.system_time = info->system_time;
    sample.tsc_to_system_mul = info->tsc_to_system_mul;
    sample.tsc_shift = info->tsc_shift;
    sample.flags = info->flags;
    return sample;
}
} // namespace

probe_result detect() noexcept
{
    const auto basic = cpu_info::read_cpuid(1);
    if ((basic.ecx & (1u << 31)) == 0)
        return {};

    const auto vendor = cpu_info::read_cpuid(kvm_clock_vendor_leaf);
    cpuid_snapshot snapshot;
    snapshot.hypervisor_present = true;
    snapshot.max_leaf = vendor.eax;
    snapshot.vendor_ebx = vendor.ebx;
    snapshot.vendor_ecx = vendor.ecx;
    snapshot.vendor_edx = vendor.edx;
    if (snapshot.max_leaf >= kvm_clock_feature_leaf)
        snapshot.feature_eax = cpu_info::read_cpuid(kvm_clock_feature_leaf).eax;
    return probe(snapshot);
}

bool clock::read_sample(const volatile pvclock_vcpu_time_info *info, pvclock_sample &sample, u32 &retries,
                        u64 *sample_tsc) noexcept
{
    retries = 0;
    if (info == nullptr)
        return false;

    for (u32 attempt = 0; attempt < max_sample_retries; attempt++)
    {
        const u32 first_version = info->version;
        if ((first_version & 1u) != 0)
        {
            retries++;
            cpu_pause();
            continue;
        }

        const auto candidate = copy_sample(info);
        const u64 candidate_tsc = read_tsc_ordered();
        compiler_barrier();
        const u32 second_version = info->version;
        if (first_version == second_version && (second_version & 1u) == 0)
        {
            sample = candidate;
            if (sample_tsc != nullptr)
                *sample_tsc = candidate_tsc;
            return valid_sample(candidate);
        }
        retries++;
        cpu_pause();
    }
    return false;
}

bool clock::start_cpu() noexcept
{
    if (started())
        return true;
    if (selection_ == policy::disabled)
        return false;

    const auto features = detect();
    if (!features.available)
        return false;

    page_ = memory::malloc_page();
    if (page_ == nullptr)
        return false;
    memset(page_, 0, memory::page_size);
    info_ = reinterpret_cast<volatile pvclock_vcpu_time_info *>(page_);

    const u64 guest_physical_address = reinterpret_cast<u64>(memory::va2pa(page_)());
    _wrmsr(msr_kvm_system_time_new, guest_physical_address | 1u);

    pvclock_sample sample;
    u32 retries = 0;
    if (!read_sample(info_, sample, retries) || !valid_sample(sample))
    {
        increment_saturated(invalid_samples_);
        stop_cpu();
        return false;
    }
    for (u32 i = 0; i < retries; i++)
        increment_saturated(seqlock_retries_);
    cached_sample_ = sample;
    cross_cpu_monotonic_ = features.cross_cpu_monotonic && (sample.flags & 1u) != 0;
    increment_saturated(stable_samples_);
    if ((sample.flags & 2u) != 0)
        increment_saturated(paused_samples_);
    return true;
}

void clock::stop_cpu() noexcept
{
    if (page_ == nullptr)
        return;
    _wrmsr(msr_kvm_system_time_new, 0);
    memory::free_page(page_);
    page_ = nullptr;
    info_ = nullptr;
    cross_cpu_monotonic_ = false;
}

timeclock::nanosecond_t clock::now_ns() noexcept
{
    if (info_ == nullptr)
        return last_returned_ns_;

    pvclock_sample sample;
    u32 retries = 0;
    u64 sample_tsc = 0;
    u64 candidate = 0;
    if (read_sample(info_, sample, retries, &sample_tsc) && try_compute_time(sample, sample_tsc, candidate))
    {
        retry_streak_.store(0, std::memory_order_relaxed);
        for (u32 i = 0; i < retries; i++)
            increment_saturated(seqlock_retries_);
        cached_sample_ = sample;
        cross_cpu_monotonic_ = cross_cpu_monotonic_ && (sample.flags & 1u) != 0;
        increment_saturated(stable_samples_);
        if ((sample.flags & 2u) != 0)
            increment_saturated(paused_samples_);
    }
    else if (try_compute_time(cached_sample_, read_tsc_ordered(), candidate))
    {
        const u64 streak = retry_streak_.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((streak & (retry_warning_interval - 1)) == 0)
            retry_warning_pending_.store(true, std::memory_order_release);
        for (u32 i = 0; i < retries; i++)
            increment_saturated(seqlock_retries_);
        increment_saturated(cached_fallbacks_);
    }
    else
    {
        const u64 streak = retry_streak_.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((streak & (retry_warning_interval - 1)) == 0)
            retry_warning_pending_.store(true, std::memory_order_release);
        for (u32 i = 0; i < retries; i++)
            increment_saturated(seqlock_retries_);
        increment_saturated(invalid_samples_);
        return last_returned_ns_;
    }

    if (candidate < last_returned_ns_)
    {
        increment_saturated(backward_clamps_);
        candidate = last_returned_ns_;
    }
    last_returned_ns_ = candidate;
    return candidate;
}

health_counters clock::health() const noexcept
{
    return {stable_samples_.load(std::memory_order_relaxed),   seqlock_retries_.load(std::memory_order_relaxed),
            cached_fallbacks_.load(std::memory_order_relaxed), backward_clamps_.load(std::memory_order_relaxed),
            invalid_samples_.load(std::memory_order_relaxed),  paused_samples_.load(std::memory_order_relaxed)};
}

bool clock::take_retry_warning() noexcept { return retry_warning_pending_.exchange(false, std::memory_order_acq_rel); }

clock *make_event_clock(policy selection) noexcept
{
    return memory::New<clock>(memory::KernelCommonAllocatorV, selection);
}

} // namespace arch::kvm_pvclock
