#pragma once

#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/terminal.hpp"

namespace dev::tty
{
/// Minimal kernel console stream used only for early/emergency diagnostics.
/// It has no line discipline and is never a job-control terminal.
class console_pseudo_t final : public fs::vfs::pseudo_t
{
  public:
    explicit console_pseudo_t(int term_index)
        : term_index_(term_index)
    {
    }

    i64 write(const byte *data, u64 size, flag_t flags) override;
    i64 read(byte *data, u64 max_size, flag_t flags) override;
    void close() override;

  private:
    int term_index_;
};
} // namespace dev::tty
