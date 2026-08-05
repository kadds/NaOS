#include "kernel/dev/framebuffer.hpp"
#include "kernel/errno.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/terminal.hpp"

namespace dev::framebuffer
{
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

} // namespace dev::framebuffer
