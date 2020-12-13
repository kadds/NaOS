#include "kernel/ksybs.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/trace.hpp"
namespace ksybs
{
struct item
{
    u64 addr;
    u64 offset;
};

struct header
{
    u64 magic;
    u64 version;
    u64 count;
    bool is_valid() { return magic == 0xF0EAEACC && version <= 1; }
    item *first() { return (item *)((byte *)this + sizeof(u64) * 3); }
    item *last() { return (item *)((byte *)this + sizeof(u64) * 3 + sizeof(item) * count); }
    byte *offset_start() { return (byte *)(last()); }
};

header *file_header = nullptr;

void init()
{
    trace::debug("Kernel symbols init");
    auto f = fs::vfs::open("/data/ksybs", fs::vfs::global_root, fs::vfs::global_root, fs::mode::read | fs::mode::bin,
                           fs::path_walk_flags::file);
    if (f == nullptr)
    {
        trace::info("Loading kernel symbols file failed.");
    }
    u64 size = fs::vfs::size(f);
    file_header = (header *)memory::KernelVirtualAllocatorV->allocate(size, 8);

    if (unlikely(f->read((byte *)file_header, size, 0) != (i64)size))
    {
        memory::KernelVirtualAllocatorV->deallocate(file_header);
        trace::info("Reading kernel symbols file failed.");
        file_header = nullptr;
        f->close();
        return;
    }
    f->close();

    if (!file_header->is_valid())
    {
        trace::info("Reading kernel symbols file failed.", " magic: ", file_header->magic,
                    ". version: ", file_header->version);
        memory::KernelVirtualAllocatorV->deallocate(file_header);
        file_header = nullptr;
        return;
    }
}

const char *get_symbol_name(void *address)
{
    u64 addr = (u64)address;
    if (unlikely(file_header == nullptr))
    {
        return nullptr;
    }
    item *head = file_header->first(), *tail = file_header->last();
    item *mid = head + file_header->count / 2;
    while (head < tail - 1)
    {
        if (mid->addr < addr)
        {
            if ((mid + 1)->addr > addr)
            {
                auto start = (const char *)(file_header->offset_start());
                return start + mid->offset + 1;
            }
            head = mid;
        }
        else
        {
            tail = mid;
        }
        mid = head + ((u64)tail - (u64)head) / sizeof(item) / 2;
    }
    return nullptr;
}
} // namespace ksybs
