#pragma once
#include "../../wait.hpp"
#include "freelibcxx/circular_buffer.hpp"
#include "kernel/common.hpp"
#include "kernel/errno.hpp"
namespace dev::tty
{
struct termios_t;
struct winsize_t;
} // namespace dev::tty

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
    virtual bool native_tty_get_attributes(dev::tty::termios_t &attributes)
    {
        (void)attributes;
        return false;
    }
    virtual bool native_tty_set_attributes(const dev::tty::termios_t &attributes)
    {
        (void)attributes;
        return false;
    }
    virtual bool native_tty_get_winsize(dev::tty::winsize_t &size)
    {
        (void)size;
        return false;
    }
    virtual bool native_tty_set_winsize(const dev::tty::winsize_t &size)
    {
        (void)size;
        return false;
    }
    virtual i64 native_tty_flush(i32 queue)
    {
        (void)queue;
        return -1;
    }
    virtual bool native_pty_get_number(u32 &number)
    {
        (void)number;
        return false;
    }
    virtual bool native_pty_set_locked(bool locked)
    {
        (void)locked;
        return false;
    }
    virtual i64 native_tty_attach(bool force)
    {
        (void)force;
        return ENOTTY;
    }
    virtual i64 native_tty_get_pgrp(u32 &group)
    {
        (void)group;
        return ENOTTY;
    }
    virtual i64 native_tty_set_pgrp(u32 group)
    {
        (void)group;
        return ENOTTY;
    }
    virtual i64 native_tty_get_sid(u32 &session)
    {
        (void)session;
        return ENOTTY;
    }
    virtual i64 native_tty_detach() { return ENOTTY; }
    virtual i64 native_tty_get_input(u64 &count)
    {
        (void)count;
        return ENOTTY;
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
