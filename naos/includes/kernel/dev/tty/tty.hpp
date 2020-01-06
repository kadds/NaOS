#pragma once
#include "../../fs/vfs/pseudo.hpp"
#include "common.hpp"

namespace dev::tty
{
class tty_pseudo_t : public fs::vfs::pseudo_t
{
  private:
    util::circular_buffer<byte> buffer;
    task::wait_queue wait_queue;
    std::atomic_int line_count;
    friend bool tty_read_func(u64);

  public:
    u64 write(const byte *data, u64 size, flag_t flags) override;
    u64 read(byte *data, u64 max_size, flag_t flags) override;

    u64 write_to_buffer(const byte *data, u64 size, flag_t flags);

    tty_pseudo_t(u64 size = 512)
        : buffer(memory::KernelMemoryAllocatorV, size)
        , wait_queue(memory::KernelCommonAllocatorV)
        , line_count(0)
    {
    }
};
} // namespace dev::tty
