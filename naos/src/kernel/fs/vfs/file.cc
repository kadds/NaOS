#include "kernel/fs/vfs/file.hpp"
#include "kernel/common.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/vfs/dentry.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/pseudo.hpp"
#include "kernel/fs/vfs/super_block.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include <limits>

namespace fs::vfs
{
namespace
{
class file_lock_guard
{
  public:
    explicit file_lock_guard(lock::mutex_t &lock)
        : lock_(lock)
    {
        lock_.lock();
    }

    ~file_lock_guard() { lock_.unlock(); }

    file_lock_guard(const file_lock_guard &) = delete;
    file_lock_guard &operator=(const file_lock_guard &) = delete;

  private:
    lock::mutex_t &lock_;
};
} // namespace

file::~file()
{
    if (entry == nullptr)
        return;

    auto *opened_entry = entry;
    auto *node = opened_entry->get_inode();
    if (node == nullptr)
    {
        entry = nullptr;
        return;
    }

    const bool unlink_on_close = mode & mode::unlink_on_close;
    bool last_open_reference = false;
    if (open_reference)
    {
        last_open_reference = node->release_open_reference();
        open_reference = false;
    }

    auto *pd = node->get_pseudo_data();
    if (pd != nullptr && (last_open_reference || pd->close_per_open()))
        pd->close_with_flags(mode);

    // An unlinked inode is reaped by the last file description that references it.
    // Calling unlink again is intentional: vfs::unlink treats link_count == 0 as a
    // reap request and does not decrement it a second time.
    if (unlink_on_close || node->get_link_count() == 0)
        unlink(opened_entry);

    entry = nullptr;
}

int file::open(dentry *entry, flag_t mode)
{
    return open(entry, mode, task::access_context{});
}

int file::open(dentry *entry, flag_t mode, const task::access_context &context)
{
    if (entry == nullptr || entry->get_inode() == nullptr || open_reference)
        return -1;

    this->entry = entry;
    this->mode = mode;
    this->offset = 0;

    auto *node = entry->get_inode();
    node->acquire_open_reference();
    {
        auto *pd = node->get_pseudo_data();
        if (pd != nullptr)
        {
            const int result = pd->open(mode, context);
            if (result != 0)
            {
                node->release_open_reference();
                this->entry = nullptr;
                this->mode = 0;
                return result;
            }
        }
    }

    open_reference = true;
    return 0;
}

void file::seek(i64 offset)
{
    file_lock_guard guard(io_lock_);
    this->offset += offset;
}

void file::move(i64 where)
{
    file_lock_guard guard(io_lock_);
    this->offset = where;
}

i64 file::current_offset()
{
    file_lock_guard guard(io_lock_);
    return offset;
}

flag_t file::get_mode() const
{
    file_lock_guard guard(io_lock_);
    return mode;
}

int file::native_sync()
{
    flush();
    return 0;
}

bool file::native_truncate(u64 length)
{
    file_lock_guard guard(io_lock_);
    if (entry == nullptr || entry->get_inode() == nullptr || (mode & fs::mode::write) == 0)
        return false;
    entry->get_inode()->set_size(length);
    if (offset > static_cast<i64>(length))
        offset = static_cast<i64>(length);
    return true;
}

bool file::native_allocate(u64 allocation_offset, u64 length)
{
    file_lock_guard guard(io_lock_);
    if (allocation_offset > std::numeric_limits<u64>::max() - length)
        return false;
    if (entry == nullptr || entry->get_inode() == nullptr || (mode & fs::mode::write) == 0)
        return false;
    const u64 end = allocation_offset + length;
    if (entry->get_inode()->get_size() < end)
        entry->get_inode()->set_size(end);
    return true;
}

bool file::native_set_flags(flag_t flags)
{
    file_lock_guard guard(io_lock_);
    constexpr flag_t mutable_flags = fs::mode::append | fs::mode::no_block;
    if ((flags & ~mutable_flags) != 0)
        return false;
    mode = (mode & ~mutable_flags) | (flags & mutable_flags);
    return true;
}

u64 file::size() const
{
    file_lock_guard guard(io_lock_);
    return entry == nullptr || entry->get_inode() == nullptr ? 0 : entry->get_inode()->get_size();
}

dentry *file::get_entry() const { return entry; }

i64 file::read(byte *ptr, u64 max_size, flag_t flags, pseudo_t::interruption_check interrupted,
               pseudo_t::wait_queue_registration register_wait_queue)
{
    file_lock_guard guard(io_lock_);
    if (entry == nullptr || entry->get_inode() == nullptr || (mode & fs::mode::read) == 0)
        return EBADF;
    if (max_size != 0 && ptr == nullptr)
        return EFAULT;
    if (max_size == 0)
        return 0;
    auto type = entry->get_inode()->get_type();
    if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory || type == fs::inode_type_t::symbolink)
        return iread(offset, ptr, max_size, flags);

    auto *pd = entry->get_inode()->get_pseudo_data();
    if (pd == nullptr)
        return EFAILED;
    return pd->read_at_interruptible(offset, ptr, max_size, mode | flags, interrupted, register_wait_queue);
}

i64 file::write(const byte *ptr, u64 size, flag_t flags, pseudo_t::interruption_check interrupted,
                pseudo_t::wait_queue_registration register_wait_queue)
{
    file_lock_guard guard(io_lock_);
    if (entry == nullptr || entry->get_inode() == nullptr || (mode & fs::mode::write) == 0)
        return EBADF;
    if (size != 0 && ptr == nullptr)
        return EFAULT;
    if (size == 0)
        return 0;
    if ((mode & fs::mode::append) != 0)
        offset = static_cast<i64>(entry->get_inode()->get_size());
    auto type = entry->get_inode()->get_type();
    if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory || type == fs::inode_type_t::symbolink)
        return iwrite(offset, ptr, size, flags);

    auto *pd = entry->get_inode()->get_pseudo_data();
    if (pd == nullptr)
        return -1;
    return pd->write_at_interruptible(offset, ptr, size, mode | flags, interrupted, register_wait_queue);
}

i64 file::pread(i64 offset, byte *ptr, u64 max_size, flag_t flags, pseudo_t::interruption_check interrupted,
                pseudo_t::wait_queue_registration register_wait_queue)
{
    pseudo_t *pd = nullptr;
    flag_t io_mode = 0;
    {
        file_lock_guard guard(io_lock_);
        if (entry == nullptr || entry->get_inode() == nullptr || (mode & fs::mode::read) == 0)
            return EBADF;
        if (offset < 0)
            return EINVAL;
        if (max_size != 0 && ptr == nullptr)
            return EFAULT;
        if (max_size == 0)
            return 0;
        auto type = entry->get_inode()->get_type();
        if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory ||
            type == fs::inode_type_t::symbolink)
            return iread(offset, ptr, max_size, flags);

        pd = entry->get_inode()->get_pseudo_data();
        if (pd == nullptr)
            return -1;
        io_mode = mode | flags;
    }
    i64 current_offset = offset;
    return pd->read_at_interruptible(current_offset, ptr, max_size, io_mode, interrupted, register_wait_queue);
}

i64 file::pwrite(i64 offset, const byte *ptr, u64 size, flag_t flags, pseudo_t::interruption_check interrupted,
                 pseudo_t::wait_queue_registration register_wait_queue)
{
    pseudo_t *pd = nullptr;
    flag_t io_mode = 0;
    {
        file_lock_guard guard(io_lock_);
        if (entry == nullptr || entry->get_inode() == nullptr || (mode & fs::mode::write) == 0)
            return EBADF;
        if (offset < 0)
            return EINVAL;
        if (size != 0 && ptr == nullptr)
            return EFAULT;
        if (size == 0)
            return 0;

        auto type = entry->get_inode()->get_type();
        if (type == fs::inode_type_t::file || type == fs::inode_type_t::directory ||
            type == fs::inode_type_t::symbolink)
            return iwrite(offset, ptr, size, flags);

        pd = entry->get_inode()->get_pseudo_data();
        if (pd == nullptr)
            return -1;
        io_mode = mode | flags;
    }
    i64 current_offset = offset;
    return pd->write_at_interruptible(current_offset, ptr, size, io_mode, interrupted, register_wait_queue);
}

pseudo_t *file::get_pseudo()
{
    if (entry == nullptr || entry->get_inode() == nullptr)
        return nullptr;
    return entry->get_inode()->get_pseudo_data();
}

const pseudo_t *file::get_pseudo() const
{
    if (entry == nullptr || entry->get_inode() == nullptr)
        return nullptr;
    return entry->get_inode()->get_pseudo_data();
}

na_signal_t file::capability_signals() const
{
    if (entry == nullptr || entry->get_inode() == nullptr)
        return NA_SIGNAL_PEER_CLOSED;

    // pseudo_t poll values intentionally use the traditional poll bit
    // layout. Translate them to the stable Object-call signal vocabulary.
    constexpr u32 poll_readable = 0x001;
    constexpr u32 poll_writable = 0x004;
    constexpr u32 poll_error = 0x008;
    constexpr u32 poll_hangup = 0x010;

    const auto *pseudo = get_pseudo();
    if (pseudo != nullptr)
    {
        const u32 events = pseudo->poll_events();
        na_signal_t signals = 0;
        if ((events & poll_readable) != 0)
            signals |= NA_SIGNAL_READABLE;
        if ((events & poll_writable) != 0)
            signals |= NA_SIGNAL_WRITABLE;
        if ((events & (poll_error | poll_hangup)) != 0)
            signals |= NA_SIGNAL_PEER_CLOSED;
        return signals;
    }

    // Regular files and directories do not block in poll: reads at EOF and
    // directory iteration completion are both immediately observable.
    na_signal_t signals = NA_SIGNAL_READABLE;
    if ((mode & fs::mode::write) != 0)
        signals |= NA_SIGNAL_WRITABLE;
    return signals;
}

} // namespace fs::vfs
