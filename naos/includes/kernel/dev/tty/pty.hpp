#pragma once

#include "kernel/dev/tty/tty_core.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include <atomic>

namespace dev::tty
{
class pty_pair;

enum class pty_endpoint_role : u8
{
    master,
    slave,
};

class pty_endpoint : public fs::vfs::pseudo_t
{
  public:
    int open(flag_t flags) override;
    i64 read(byte *data, u64 max_size, flag_t flags) override;
    i64 write(const byte *data, u64 size, flag_t flags) override;
    bool native_tty_get_attributes(termios_t &attributes) override;
    bool native_tty_set_attributes(const termios_t &attributes) override;
    bool native_tty_get_winsize(winsize_t &size) override;
    bool native_tty_set_winsize(const winsize_t &size) override;
    i64 native_tty_flush(i32 queue) override;
    i64 native_tty_attach(bool force) override;
    i64 native_tty_get_pgrp(u32 &group) override;
    i64 native_tty_set_pgrp(u32 group) override;
    i64 native_tty_get_sid(u32 &session) override;
    i64 native_tty_detach() override;
    i64 native_tty_get_input(u64 &count) override;
    bool native_pty_get_number(u32 &number) override;
    bool native_pty_set_locked(bool locked) override;
    void close() override;

    u32 poll_events() const override;
    bool owned_by_inode() const override { return false; }
    pty_endpoint_role role() const { return role_; }
    pty_pair *pair() const { return pair_; }

  protected:
    pty_endpoint(pty_pair *pair, pty_endpoint_role role)
        : pair_(pair)
        , role_(role)
    {
    }

  private:
    bool can_operate() const;

    pty_pair *pair_;
    pty_endpoint_role role_;
};

class pty_master_endpoint final : public pty_endpoint
{
  public:
    explicit pty_master_endpoint(pty_pair *pair)
        : pty_endpoint(pair, pty_endpoint_role::master)
    {
    }
};

class pty_slave_endpoint final : public pty_endpoint
{
  public:
    explicit pty_slave_endpoint(pty_pair *pair)
        : pty_endpoint(pair, pty_endpoint_role::slave)
    {
    }
};

class pty_pair final
{
    friend class pty_endpoint;

  public:
    explicit pty_pair(u32 index, u64 input_buffer_size = 4096, u64 output_buffer_size = 4096,
                      freelibcxx::Allocator *allocator = nullptr);
    ~pty_pair() = default;

    pty_pair(const pty_pair &) = delete;
    pty_pair &operator=(const pty_pair &) = delete;

    u32 index() const { return index_; }
    tty_core &core() { return core_; }
    const tty_core &core() const { return core_; }
    pty_master_endpoint &master() { return master_; }
    pty_slave_endpoint &slave() { return slave_; }
    const pty_master_endpoint &master() const { return master_; }
    const pty_slave_endpoint &slave() const { return slave_; }

    bool master_open() const { return master_open_; }
    bool slave_open() const { return slave_open_; }
    bool slave_locked() const { return slave_locked_; }
    bool activate_slave();
    void set_slave_locked(bool locked) { slave_locked_ = locked; }
    void close_master();
    void close_slave();

  private:
    u32 index_;
    tty_core core_;
    pty_master_endpoint master_;
    pty_slave_endpoint slave_;
    std::atomic_bool master_open_;
    std::atomic_bool slave_open_;
    std::atomic_bool slave_locked_;
    std::atomic_uint32_t master_references_;
    std::atomic_uint32_t slave_references_;

    bool open_master_reference();
    bool open_slave_reference();
};
} // namespace dev::tty
