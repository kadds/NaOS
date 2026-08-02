#pragma once

#include "kernel/dev/tty/tty_core.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/terminal.hpp"

namespace dev::tty
{
/// Compatibility console pseudo device. It keeps the old input_task-facing API
/// while using the common tty core for input editing and echo.
class tty_pseudo_t final : public fs::vfs::pseudo_t
{
  public:
    i64 write(const byte *data, u64 size, flag_t flags) override;
    i64 read(byte *data, u64 max_size, flag_t flags) override;
    i64 ioctl(fs::vfs::ioctl_context &context) override;
    u32 poll_events() const override { return core_.input_poll_events() | core_.output_poll_events(); }

    u64 write_to_buffer(const byte *data, u64 size, flag_t flags);
    void send_EOF();
    void close() override;

    tty_core &core() { return core_; }
    const tty_core &core() const { return core_; }
    bool readable() const { return core_.input_readable(); }

    tty_pseudo_t(int term_index, u64 size = 4096)
        : core_(size, size)
        , master_(core_)
        , term_index_(term_index)
    {
    }

  private:
    void render_master_output();

    tty_core core_;
    tty_master master_;
    int term_index_;
};

bool tty_read_func(u64 data);
} // namespace dev::tty
