#pragma once
#include "kernel/kobject.hpp"

namespace dev::framebuffer
{
/// Kernel-owned framebuffer device service.  It holds the physical display
/// geometry and hands the display memory out as a direct-mapped MemoryObject
/// to exactly one display writer at a time (NA_DISPLAY_RIGHT_WRITER gates the
/// protocol).  `physical_base` must be page-aligned; `kernel_view` is the
/// kernel's cached mapping of the same range.
class framebuffer_service final : public kobject
{
  public:
    framebuffer_service(phy_addr_t physical_base, void *kernel_view, u64 frame_bytes, u64 width, u64 height,
                        u64 pitch, u64 bpp, u64 type)
        : kobject(kobject::type_e::framebuffer)
        , physical_base_(physical_base)
        , kernel_view_(kernel_view)
        , frame_bytes_(frame_bytes)
        , width_(width)
        , height_(height)
        , pitch_(pitch)
        , bpp_(bpp)
        , type_(type)
    {
    }

    static type_e type_of() { return type_e::framebuffer; }

    phy_addr_t physical_base() const { return physical_base_; }
    void *kernel_view() const { return kernel_view_; }
    u64 frame_bytes() const { return frame_bytes_; }
    u64 width() const { return width_; }
    u64 height() const { return height_; }
    u64 pitch() const { return pitch_; }
    u64 bpp() const { return bpp_; }
    u64 type() const { return type_; }

  private:
    phy_addr_t physical_base_;
    void *kernel_view_;
    u64 frame_bytes_;
    u64 width_;
    u64 height_;
    u64 pitch_;
    u64 bpp_;
    u64 type_;
};

} // namespace dev::framebuffer
