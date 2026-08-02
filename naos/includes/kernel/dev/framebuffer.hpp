#pragma once
#include "kernel/framebuffer.hpp"
#include "kernel/fs/vfs/pseudo.hpp"

namespace term
{
class terminal_manager;
}

namespace dev::framebuffer
{
class framebuffer_pseudo_t final : public fs::vfs::pseudo_t
{
  public:
    explicit framebuffer_pseudo_t(term::terminal_manager *manager)
        : manager_(manager)
    {
    }

    i64 write(const byte *data, u64 size, flag_t flags) override;
    i64 read(byte *data, u64 max_size, flag_t flags) override;
    i64 write_at(i64 &offset, const byte *data, u64 size, flag_t flags) override;
    i64 read_at(i64 &offset, byte *data, u64 max_size, flag_t flags) override;
    i64 ioctl(fs::vfs::ioctl_context &context) override;
    bool supports_physical_mmap() const override { return true; }
    bool get_physical_mmap(u64 offset, u64 length, phy_addr_t &physical_address) const override;
    void close() override {}

  private:
    i64 fill_var_screeninfo(fb::ioctl::var_screeninfo *info) const;
    i64 fill_fix_screeninfo(fb::ioctl::fix_screeninfo *info) const;

    term::terminal_manager *manager_;
};
} // namespace dev::framebuffer
