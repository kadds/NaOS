#pragma once
#include "../../fs/vfs/pseudo.hpp"
#include "common.hpp"

namespace dev::tty
{
class tty_pseudo_t : public fs::vfs::pseudo_t
{
  private:
    util::circular_buffer<byte> buffer;
    task::wait_queue_t wait_queue;
    std::atomic_int line_count;
    std::atomic_int eof_count;

    friend bool tty_read_func(u64);

  public:
    i64 write(const byte *data, u64 size, flag_t flags) override;
    i64 read(byte *data, u64 max_size, flag_t flags) override;

    u64 write_to_buffer(const byte *data, u64 size, flag_t flags);
    void send_EOF();

    void close() override;

    tty_pseudo_t(u64 size = 512)
        : buffer(memory::MemoryAllocatorV, size)
        , line_count(0)
        , eof_count(0)
    {
    }
};
} // namespace dev::tty
