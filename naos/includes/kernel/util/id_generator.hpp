#pragma once
#include "../lock.hpp"
#include "../mm/new.hpp"
#include "../ucontext.hpp"
#include "bit_set.hpp"
#include "common.hpp"
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
            cur = start.load(std::memory_order_release);
        } while (!start.compare_exchange_strong(cur, cur + step, std::memory_order_acquire));
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
    bit_set bitmap;

  public:
    id_generator(u64 max)
        : max(max)
        , last_index(0)
        , bitmap(memory::KernelCommonAllocatorV, max)
    {
        bitmap.clean_all();
    };

    u64 next()
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (!bitmap.get(last_index))
        {
            bitmap.set(last_index);
            return last_index;
        }
        u64 index = bitmap.scan_zero();
        if (likely(index < max))
        {
            bitmap.set(index);
            return index;
        }
        return null_id;
    }

    void tag(u64 i)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(i == null_id))
            return;
        bitmap.set(i);
    }

    void collect(u64 i)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(i == null_id))
            return;
        if (!bitmap.get(last_index))
            last_index = last_index > i ? last_index : i; /// save maximum
        else
            last_index = i;

        bitmap.clean(i);
    };

    bool has_use(u64 i)
    {
        uctx::RawSpinLockUninterruptibleContext icu(spinlock);
        if (unlikely(i == null_id))
            return false;
        return bitmap.get(last_index);
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
        bit_set *bitmap;
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
                    pack[i].bitmap = memory::New<bit_set>(memory::KernelCommonAllocatorV,
                                                          memory::KernelCommonAllocatorV, pack[0].rest);
                    pack[i].bitmap->clean_all();
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
        pack[0].bitmap =
            memory::New<bit_set>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV, pack[0].rest);
        pack[0].bitmap->clean_all();
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
        if (!current_pack->bitmap->get(current_pack->last_index))
        {
            auto index = current_pack->last_index;
            current_pack->bitmap->set(index);
            current_pack->rest--;
            return index;
        }
        auto index = current_pack->bitmap->scan_zero();
        if (likely(index < current_pack->max))
        {
            current_pack->bitmap->set(index);
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
                if (!pack[i].bitmap->get(id - pack[i].base))
                {
                    pack[i].rest--;
                }
                pack[i].bitmap->set(id - pack[i].base);
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
                pack[i].bitmap->clean(id - pack[i].base);
                pack[i].rest++;
                if (!pack[i].bitmap->get(pack[i].last_index))
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
                return pack[i].bitmap->get(i);
            }
        }
        return false;
    }
};
} // namespace util
