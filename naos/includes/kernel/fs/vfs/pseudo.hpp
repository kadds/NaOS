#pragma once
#include "../../util/circular_buffer.hpp"
#include "../../wait.hpp"
#include "common.hpp"
namespace fs::vfs
{
class pseudo_t
{
  public:
    virtual u64 write(const byte *data, u64 size, flag_t flags) = 0;
    virtual u64 read(byte *data, u64 max_size, flag_t flags) = 0;
};

class pseudo_pipe_t : public pseudo_t
{
    util::circular_buffer<byte> buffer;
    task::wait_queue wait_queue;

  public:
    u64 write(const byte *data, u64 size, flag_t flags) override;
    u64 read(byte *data, u64 max_size, flag_t flags) override;
    pseudo_pipe_t(u64 size = 512)
        : buffer(memory::KernelMemoryAllocatorV, size)
        , wait_queue(memory::KernelCommonAllocatorV)
    {
    }
};

} // namespace fs::vfs
