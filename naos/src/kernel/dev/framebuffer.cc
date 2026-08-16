#include "kernel/dev/framebuffer.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/vm.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal.hpp"
#include "kernel/trace.hpp"

namespace dev::framebuffer
{
namespace
{
framebuffer_pseudo_t *global_framebuffer = nullptr;
} // namespace

framebuffer_pseudo_t::framebuffer_pseudo_t(fb::framebuffer_backend *backend)
    : backend_(backend)
{
    global_framebuffer = this;
}

int framebuffer_pseudo_t::open(flag_t flags, const task::access_context &context)
{
    if ((flags & fs::mode::write) == 0)
        return 0;
    if (!term::framebuffer_user_enabled())
        return EBUSY;
    // A writable framebuffer is a privileged frontend resource. The public
    // terminal factory is deliberately not enough; callers need the separate
    // console frontend capability.
    auto *process = context.caller;
    if (process == nullptr || !process->resource.has_native_object_type(kobject::type_e::console_frontend))
        return EACCES;

    uctx::RawSpinLockUninterruptibleContext guard(writer_lock_);
    if (writer_active_)
        return EBUSY;

    writer_active_ = true;
    writer_process_ = process;
    writer_open_references_ = 1;
    term::set_framebuffer_user_writer(true);
    return 0;
}

bool framebuffer_pseudo_t::allow_mapping(bool writable, bool shared) const
{
    if (!writable)
        return true;
    auto *process = task::current_process();
    if (process == nullptr || !process->resource.has_native_object_type(kobject::type_e::console_frontend))
        return false;

    uctx::RawSpinLockUninterruptibleContext guard(writer_lock_);
    return writer_active_ && writer_process_ == process && shared;
}

bool framebuffer_pseudo_t::allow_fork_mapping(bool writable) const
{
    // A child must not inherit the physical scanout mapping or the parent's
    // writer lease.  It may still inherit a read-only framebuffer view.
    return !writable;
}

void framebuffer_pseudo_t::release_writer_if_idle_locked()
{
    if (writer_open_references_ == 0 && writable_mappings_ == 0 && writer_active_)
    {
        writer_active_ = false;
        writer_process_ = nullptr;
        term::set_framebuffer_user_writer(false);
    }
}

void framebuffer_pseudo_t::force_offline()
{
    task::process_t *process = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext guard(writer_lock_);
        process = writer_process_;
    }
    if (process == nullptr || process == task::get_init_process() ||
        (process->attributes.load(std::memory_order_acquire) & task::process_attributes::no_thread) != 0)
        return;

    trace::warning("forcing framebuffer owner offline pid=", process->pid);
    // Do not edit a foreign address space from this CPU: this kernel has no
    // TLB shootdown primitive yet. Process teardown unmaps the framebuffer
    // and releases the writer lease on the owning process's CPU.
    task::exit_process(process, -EIO, 0);
}

bool force_user_offline()
{
    if (global_framebuffer == nullptr)
        return false;
    global_framebuffer->force_offline();
    return true;
}

void framebuffer_pseudo_t::close() {}

void framebuffer_pseudo_t::close_with_flags(flag_t flags)
{
    if ((flags & fs::mode::write) == 0)
        return;
    uctx::RawSpinLockUninterruptibleContext guard(writer_lock_);
    if (writer_open_references_ == 0)
        return;

    --writer_open_references_;
    release_writer_if_idle_locked();
}

void framebuffer_pseudo_t::on_mapping_created(flag_t flags)
{
    if ((flags & memory::vm::flags::writeable) != 0)
    {
        uctx::RawSpinLockUninterruptibleContext guard(writer_lock_);
        ++writable_mappings_;
    }
}

void framebuffer_pseudo_t::on_mapping_released(flag_t flags)
{
    if ((flags & memory::vm::flags::writeable) == 0)
        return;
    uctx::RawSpinLockUninterruptibleContext guard(writer_lock_);
    if (writable_mappings_ == 0)
        return;

    --writable_mappings_;
    release_writer_if_idle_locked();
}

i64 framebuffer_pseudo_t::write(const byte *data, u64 size, flag_t flags)
{
    (void)flags;
    i64 offset = 0;
    return write_at(offset, data, size, flags);
}

i64 framebuffer_pseudo_t::read(byte *data, u64 max_size, flag_t flags)
{
    (void)flags;
    i64 offset = 0;
    return read_at(offset, data, max_size, flags);
}

i64 framebuffer_pseudo_t::write_at(i64 &offset, const byte *data, u64 size, flag_t flags)
{
    (void)flags;
    if (backend_ == nullptr || (flags & fs::mode::write) == 0)
    {
        return EACCES;
    }
    return backend_->write_bytes(offset, data, size);
}

i64 framebuffer_pseudo_t::read_at(i64 &offset, byte *data, u64 max_size, flag_t flags)
{
    (void)flags;
    if (backend_ == nullptr)
    {
        return EFAILED;
    }
    return backend_->read_bytes(offset, data, max_size);
}

bool framebuffer_pseudo_t::get_physical_mmap(u64 offset, u64 length, phy_addr_t &physical_address) const
{
    if (backend_ == nullptr || backend_->fb().bbp != 32)
    {
        return false;
    }

    const auto &framebuffer = backend_->fb();
    const u64 frame_bytes = backend_->frame_bytes();
    const u64 mapped_bytes = (frame_bytes + memory::page_size - 1) & ~(memory::page_size - 1);
    if (framebuffer.physical_addr == nullptr || offset >= mapped_bytes || length > mapped_bytes - offset ||
        (offset & (memory::page_size - 1)) != 0 ||
        (reinterpret_cast<u64>(framebuffer.physical_addr()) & (memory::page_size - 1)) != 0)
    {
        return false;
    }

    physical_address = framebuffer.physical_addr + static_cast<ptrdiff_t>(offset);
    return true;
}

i64 framebuffer_pseudo_t::native_device_control(u64 request, const byte *input, u64 input_size, byte *output,
                                                u64 output_capacity, u64 &output_size)
{
    output_size = 0;
    if (backend_ == nullptr || input_size != 0)
        return EINVAL;

    const auto &framebuffer = backend_->fb();
    if (request == FBIOGET_FSCREENINFO)
    {
        if (output == nullptr || output_capacity < sizeof(fb_fix_screeninfo))
            return EOVERFLOW;
        fb_fix_screeninfo info{};
        memcpy(info.id, "NaOSFB0", 8);
        info.smem_start = reinterpret_cast<u64>(framebuffer.physical_addr());
        info.smem_len = backend_->frame_bytes();
        info.type = FB_TYPE_PACKED_PIXELS;
        info.visual = FB_VISUAL_TRUECOLOR;
        info.line_length = framebuffer.pitch;
        info.accel = FB_ACCEL_NONE;
        memcpy(output, &info, sizeof(info));
        output_size = sizeof(info);
        return 0;
    }

    if (request == FBIOGET_VSCREENINFO)
    {
        if (output == nullptr || output_capacity < sizeof(fb_var_screeninfo))
            return EOVERFLOW;
        fb_var_screeninfo info{};
        info.xres = framebuffer.width;
        info.yres = framebuffer.height;
        info.xres_virtual = framebuffer.width;
        info.yres_virtual = framebuffer.height;
        info.bits_per_pixel = framebuffer.bbp;
        info.red.offset = 16;
        info.red.length = 8;
        info.green.offset = 8;
        info.green.length = 8;
        info.blue.offset = 0;
        info.blue.length = 8;
        memcpy(output, &info, sizeof(info));
        output_size = sizeof(info);
        return 0;
    }

    return ENOTTY;
}

} // namespace dev::framebuffer
