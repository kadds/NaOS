#include "kernel/dev/framebuffer.hpp"
#include "kernel/errno.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/terminal.hpp"

namespace dev::framebuffer
{
namespace
{
constexpr u32 red_offset = 16;
constexpr u32 green_offset = 8;
constexpr u32 blue_offset = 0;
constexpr u32 alpha_offset = 24;
constexpr u32 channel_length = 8;

void set_rgb32_layout(fb::ioctl::var_screeninfo &info)
{
    info.red = {red_offset, channel_length, 0};
    info.green = {green_offset, channel_length, 0};
    info.blue = {blue_offset, channel_length, 0};
    info.transp = {alpha_offset, channel_length, 0};
}
} // namespace

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
    if (manager_ == nullptr)
    {
        return EFAILED;
    }
    return manager_->backend().write_bytes(offset, data, size);
}

i64 framebuffer_pseudo_t::read_at(i64 &offset, byte *data, u64 max_size, flag_t flags)
{
    (void)flags;
    if (manager_ == nullptr)
    {
        return EFAILED;
    }
    return manager_->backend().read_bytes(offset, data, max_size);
}

bool framebuffer_pseudo_t::get_physical_mmap(u64 offset, u64 length, phy_addr_t &physical_address) const
{
    if (manager_ == nullptr || manager_->backend().fb().bbp != 32)
    {
        return false;
    }

    const auto &framebuffer = manager_->backend().fb();
    const u64 frame_bytes = manager_->backend().frame_bytes();
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

i64 framebuffer_pseudo_t::fill_var_screeninfo(fb::ioctl::var_screeninfo *info) const
{
    if (manager_ == nullptr || manager_->backend().fb().bbp != 32 || info == nullptr)
    {
        return EFAILED;
    }

    const auto &framebuffer = manager_->backend().fb();
    *info = {};
    info->xres = framebuffer.width;
    info->yres = framebuffer.height;
    info->xres_virtual = framebuffer.width;
    info->yres_virtual = framebuffer.height;
    info->bits_per_pixel = framebuffer.bbp;
    info->vmode = 0;
    set_rgb32_layout(*info);
    return OK;
}

i64 framebuffer_pseudo_t::fill_fix_screeninfo(fb::ioctl::fix_screeninfo *info) const
{
    if (manager_ == nullptr || manager_->backend().fb().bbp != 32 || info == nullptr)
    {
        return EFAILED;
    }

    const auto &framebuffer = manager_->backend().fb();
    *info = {};
    const char name[] = "NaOS framebuffer";
    memcpy(info->id, name, sizeof(name) - 1);
    info->smem_start = reinterpret_cast<u64>(framebuffer.physical_addr());
    info->smem_len = manager_->backend().frame_bytes();
    info->type = 0;
    info->visual = 2;
    info->line_length = framebuffer.pitch;
    return OK;
}

i64 framebuffer_pseudo_t::ioctl(u64 request, u64 argument)
{
    switch (request)
    {
        case fb::ioctl::get_vscreeninfo:
            return fill_var_screeninfo(reinterpret_cast<fb::ioctl::var_screeninfo *>(argument));
        case fb::ioctl::get_fscreeninfo:
            return fill_fix_screeninfo(reinterpret_cast<fb::ioctl::fix_screeninfo *>(argument));
        case fb::ioctl::get_con2fbmap:
        {
            auto *map = reinterpret_cast<fb::ioctl::con2fbmap *>(argument);
            if (map == nullptr || manager_ == nullptr || map->console >= static_cast<u32>(manager_->total()))
            {
                return EPARAM;
            }
            map->framebuffer = 0;
            return OK;
        }
        case fb::ioctl::put_con2fbmap:
        {
            auto *map = reinterpret_cast<fb::ioctl::con2fbmap *>(argument);
            if (map == nullptr || manager_ == nullptr || map->console >= static_cast<u32>(manager_->total()) ||
                map->framebuffer != 0)
            {
                return EPARAM;
            }
            return OK;
        }
        case fb::ioctl::get_active_terminal:
        {
            auto *active = reinterpret_cast<u32 *>(argument);
            if (active == nullptr || manager_ == nullptr)
            {
                return EFAILED;
            }
            *active = manager_->term_index();
            return OK;
        }
        case fb::ioctl::set_active_terminal:
            if (manager_ == nullptr || argument >= static_cast<u64>(manager_->total()))
            {
                return EPARAM;
            }
            return manager_->switch_term(static_cast<int>(argument)) ? OK : EPARAM;
        case fb::ioctl::blank:
            return OK;
        case fb::ioctl::put_vscreeninfo:
        {
            auto *info = reinterpret_cast<fb::ioctl::var_screeninfo *>(argument);
            if (info == nullptr || manager_ == nullptr || manager_->backend().fb().bbp != 32)
            {
                return EFAILED;
            }

            const auto &framebuffer = manager_->backend().fb();
            if (info->xres != framebuffer.width || info->yres != framebuffer.height ||
                info->xres_virtual < framebuffer.width || info->yres_virtual < framebuffer.height ||
                info->bits_per_pixel != framebuffer.bbp)
            {
                return EPARAM;
            }
            return OK;
        }
        default:
            return ENOTTY;
    }
}

u64 framebuffer_pseudo_t::ioctl_arg_size(u64 request) const
{
    switch (request)
    {
        case fb::ioctl::get_vscreeninfo:
        case fb::ioctl::put_vscreeninfo:
            return sizeof(fb::ioctl::var_screeninfo);
        case fb::ioctl::get_fscreeninfo:
            return sizeof(fb::ioctl::fix_screeninfo);
        case fb::ioctl::get_con2fbmap:
        case fb::ioctl::put_con2fbmap:
            return sizeof(fb::ioctl::con2fbmap);
        case fb::ioctl::get_active_terminal:
            return sizeof(u32);
        default:
            return 0;
    }
}
} // namespace dev::framebuffer
