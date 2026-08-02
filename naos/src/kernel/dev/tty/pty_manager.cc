#include "kernel/dev/tty/pty_manager.hpp"

#include "freelibcxx/vector.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/ucontext.hpp"

namespace dev::pty
{
namespace
{
freelibcxx::vector<dev::tty::pty_pair *> *pairs;
lock::spinlock_t pairs_lock;
u32 next_index;

dev::tty::pty_pair *find_pair(u32 index)
{
    for (auto *pair : *pairs)
    {
        if (pair->index() == index)
            return pair;
    }
    return nullptr;
}
} // namespace

void init()
{
    pairs = memory::New<freelibcxx::vector<dev::tty::pty_pair *>>(memory::KernelCommonAllocatorV,
                                                                  memory::KernelCommonAllocatorV);
    next_index = 0;
}

handle_t<fs::vfs::file> open_master(flag_t mode)
{
    dev::tty::pty_pair *pair;
    {
        uctx::RawSpinLockUninterruptibleContext guard(pairs_lock);
        pair = memory::New<dev::tty::pty_pair>(memory::KernelCommonAllocatorV, next_index++);
        pairs->push_back(pair);
    }

    if ((mode & (fs::mode::read | fs::mode::write)) == 0)
        mode |= fs::mode::read | fs::mode::write;
    return fs::vfs::open_anonymous_pseudo(&pair->master(), fs::inode_type_t::chr, mode);
}

handle_t<fs::vfs::file> open_slave(u32 index, flag_t mode)
{
    dev::tty::pty_pair *pair;
    {
        uctx::RawSpinLockUninterruptibleContext guard(pairs_lock);
        pair = find_pair(index);
        if (pair == nullptr || pair->slave_locked())
            return {};
    }

    if ((mode & (fs::mode::read | fs::mode::write)) == 0)
        mode |= fs::mode::read | fs::mode::write;
    return fs::vfs::open_anonymous_pseudo(&pair->slave(), fs::inode_type_t::chr, mode);
}
} // namespace dev::pty
