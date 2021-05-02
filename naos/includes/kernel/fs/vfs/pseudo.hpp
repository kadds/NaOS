#pragma once
#include "../../util/circular_buffer.hpp"
#include "../../wait.hpp"
#include "common.hpp"
namespace fs::vfs
{
/// pseudo device interface
class pseudo_t
{
  public:
    virtual i64 write(const byte *data, u64 size, flag_t flags) = 0;
    virtual i64 read(byte *data, u64 max_size, flag_t flags) = 0;
    virtual void close() = 0;
    virtual ~pseudo_t() {}
};

class pseudo_pipe_t : public pseudo_t
{
    util::circular_buffer<byte> buffer;
    task::wait_queue_t wait_queue;
    std::atomic_bool is_close;
    friend bool pipe_write_func(u64 data);
    friend bool pipe_read_func(u64 data);

  public:
    i64 write(const byte *data, u64 size, flag_t flags) override;
    i64 read(byte *data, u64 max_size, flag_t flags) override;
    void close() override;
    pseudo_pipe_t(u64 size = 512)
        : buffer(memory::MemoryAllocatorV, size)
        , is_close(false)
    {
    }
};

} // namespace fs::vfs
