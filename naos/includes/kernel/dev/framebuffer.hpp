#pragma once
#include "kernel/framebuffer.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/lock.hpp"
#include <atomic>
#include <linux/fb.h>

namespace task
{
struct process_t;
}

namespace dev::framebuffer
{
bool force_user_offline();

/// A dedicated capability carried only by the console frontend. Keeping the
/// display authority on a distinct object prevents the public terminal
/// factory service from becoming an implicit framebuffer grant.
class console_frontend_capability final : public kobject
{
  public:
    console_frontend_capability()
        : kobject(kobject::type_e::console_frontend)
    {
    }

    static type_e type_of() { return type_e::console_frontend; }
};

class framebuffer_pseudo_t final : public fs::vfs::pseudo_t
{
  public:
    explicit framebuffer_pseudo_t(fb::framebuffer_backend *backend);

    i64 write(const byte *data, u64 size, flag_t flags) override;
    i64 read(byte *data, u64 max_size, flag_t flags) override;
    i64 write_at(i64 &offset, const byte *data, u64 size, flag_t flags) override;
    i64 read_at(i64 &offset, byte *data, u64 max_size, flag_t flags) override;
    bool supports_physical_mmap() const override { return true; }
    bool allow_mapping(bool writable, bool shared) const override;
    bool allow_fork_mapping(bool writable) const override;
    bool get_physical_mmap(u64 offset, u64 length, phy_addr_t &physical_address) const override;
    int open(flag_t flags, const task::access_context &context) override;
    void close() override;
    void close_with_flags(flag_t flags) override;
    bool close_per_open() const override { return true; }
    void on_mapping_created(flag_t flags) override;
    void on_mapping_released(flag_t flags) override;
    void force_offline();
    i64 native_device_control(u64 request, const byte *input, u64 input_size, byte *output, u64 output_capacity,
                              u64 &output_size) override;

  private:
    void release_writer_if_idle_locked();

    fb::framebuffer_backend *backend_;
    mutable lock::spinlock_t writer_lock_;
    u64 writer_open_references_ = 0;
    bool writer_active_ = false;
    u64 writable_mappings_ = 0;
    task::process_t *writer_process_ = nullptr;
};
} // namespace dev::framebuffer
