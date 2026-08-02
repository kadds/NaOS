#pragma once
#include "../../wait.hpp"
#include "common.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "kernel/fs/vfs/ioctl.hpp"
namespace fs::vfs
{
/// pseudo device interface
class pseudo_t
{
  public:
    virtual int open(flag_t flags)
    {
        (void)flags;
        return 0;
    }

    virtual i64 write(const byte *data, u64 size, flag_t flags) = 0;
    virtual i64 read(byte *data, u64 max_size, flag_t flags) = 0;
    virtual i64 write_at(i64 &offset, const byte *data, u64 size, flag_t flags)
    {
        (void)offset;
        return write(data, size, flags);
    }
    virtual i64 read_at(i64 &offset, byte *data, u64 max_size, flag_t flags)
    {
        (void)offset;
        return read(data, max_size, flags);
    }
    virtual i64 ioctl(ioctl_context &context)
    {
        (void)context;
        return -1;
    }
    virtual u32 poll_events() const { return 0; }
    virtual bool owned_by_inode() const { return true; }
    virtual bool supports_physical_mmap() const { return false; }
    virtual bool get_physical_mmap(u64 offset, u64 length, phy_addr_t &physical_address) const
    {
        (void)offset;
        (void)length;
        (void)physical_address;
        return false;
    }
    virtual void close() = 0;
    virtual ~pseudo_t() {}
};

class pseudo_pipe_t : public pseudo_t
{
    freelibcxx::circular_buffer<byte> buffer;
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
