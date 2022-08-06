#pragma once
#include "../lock.hpp"
#include "../mm/new.hpp"
#include "../ucontext.hpp"
#include "common.hpp"
#include "freelibcxx/bit_set.hpp"
#include "kernel/trace.hpp"
#include <atomic>
namespace util
{
inline constexpr u64 null_id = (u64)(-1);

// A lock-free sequence generator
class seq_generator
{
  public:
    seq_generator(u64 beg, i32 step)
        : start(beg)
        , step(step)
    {
    }

    u64 next()
    {
        u64 cur;
        do
        {
            cur = start.load(std::memory_order_acquire);
        } while (!start.compare_exchange_strong(cur, cur + step, std::memory_order_release));
        return cur;
    }

    void collect(u64) {}

  private:
    std::atomic_uint64_t start;
    i32 step;
};

/// TODO cache line sync
class id_generator
{
    lock::spinlock_t spinlock;
    u64 max;
    volatile u64 last_index;
    freelibcxx::bit_set bitmap;

  public:
    id_generator(u64 max)
        : max(max)
        , last_index(0)
        , bitmap(memory::KernelCommonAllocatorV, max)
    {
        bitmap.reset_all();
    };

    u64 next()
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (!bitmap.get_bit(last_index))
        {
            bitmap.set_bit(last_index);
            return last_index;
        }
        u64 index = bitmap.scan_zero();
        if (likely(index < max))
        {
            bitmap.set_bit(index);
            return index;
        }
        return null_id;
    }

    void tag(u64 i)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(i == null_id))
            return;
        bitmap.set_bit(i);
    }

    void collect(u64 i)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(i == null_id))
            return;
        if (!bitmap.get_bit(last_index))
            last_index = last_index > i ? last_index : i; /// save maximum
        else
            last_index = i;

        bitmap.reset_bit(i);
    };

    bool has_use(u64 i)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(i == null_id))
            return false;
        return bitmap.get_bit(last_index);
    }
};
/// multi-level id generator
template <u8 levels> class id_level_generator
{
    lock::spinlock_t spinlock;
    struct level_pack
    {
        u64 max;
        u64 base;
        u64 rest;
        freelibcxx::bit_set *bitmap;
        volatile u64 last_index;
    };

    level_pack pack[levels];
    level_pack *current_pack;
    static_assert(levels != 0);
    level_pack *pick_next_pack()
    {
        for (u8 i = 0; i < levels; i++)
        {
            if (pack[i].rest != 0)
            {
                if (pack[i].bitmap == nullptr)
                {
                    pack[i].bitmap = memory::New<freelibcxx::bit_set>(memory::KernelCommonAllocatorV,
                                                                      memory::KernelCommonAllocatorV, pack[0].rest);
                    pack[i].bitmap->reset_all();
                }
                return &pack[i];
            }
        }
        return nullptr;
    }

  public:
    id_level_generator(const u64 *maximums)
    {
        u64 base = 0;
        for (u8 i = 0; i < levels; i++)
        {
            kassert(maximums[i] > base, "The maximum sequence must be monotonically increasing");
            pack[i].max = maximums[i];
            pack[i].rest = maximums[i] - base;
            pack[i].bitmap = nullptr;
            pack[i].base = base;
            base = pack[i].max;
            pack[i].last_index = 0;
        }
        pack[0].bitmap = memory::New<freelibcxx::bit_set>(memory::KernelCommonAllocatorV,
                                                          memory::KernelCommonAllocatorV, pack[0].rest);
        pack[0].bitmap->reset_all();
        current_pack = &pack[0];
    };

    u64 next()
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);

        if (unlikely(current_pack->rest == 0))
        {
            auto next_pack = pick_next_pack();
            if (unlikely(next_pack == nullptr))
                return null_id;
            current_pack = next_pack;
        }
        if (!current_pack->bitmap->get_bit(current_pack->last_index))
        {
            auto index = current_pack->last_index;
            current_pack->bitmap->set_bit(index);
            current_pack->rest--;
            return index;
        }
        auto index = current_pack->bitmap->scan_zero();
        if (likely(index < current_pack->max))
        {
            current_pack->bitmap->set_bit(index);
            current_pack->rest--;
        }
        return index;
    }

    void tag(u64 id)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(id == null_id))
            return;
        for (u8 i = 0; i < levels; i++)
        {
            if (id < pack[i].max)
            {
                if (!pack[i].bitmap->get_bit(id - pack[i].base))
                {
                    pack[i].rest--;
                }
                pack[i].bitmap->set_bit(id - pack[i].base);
                break;
            }
        }
    }

    void collect(u64 id)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(id == null_id))
            return;
        for (u8 i = 0; i < levels; i++)
        {
            if (id < pack[i].max)
            {
                pack[i].bitmap->reset_bit(id - pack[i].base);
                pack[i].rest++;
                if (!pack[i].bitmap->get_bit(pack[i].last_index))
                    pack[i].last_index = pack[i].last_index > id ? pack[i].last_index : id; /// save maximum
                else
                    pack[i].last_index = id;
                break;
            }
        }
    }

    bool has_use(u64 i)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(i == null_id))
            return false;
        for (u8 i = 0; i < levels; i++)
        {
            if (i < pack[i].max)
            {
                return pack[i].bitmap->get_bit(i);
            }
        }
        return false;
    }
};
} // namespace util
