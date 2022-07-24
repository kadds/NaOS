#include "kernel/fs/rootfs/rootfs.hpp"
#include "kernel/fs/vfs/defines.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/file_system.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/trace.hpp"
#include "kernel/util/memory.hpp"
#include "kernel/util/str.hpp"
namespace fs::rootfs
{
file_system *global_root_file_system;

void tar_loader(byte *start_root_image, u64 size);

void init(byte *start_root_image, u64 size)
{
    trace::debug("Root file system init");
    if (start_root_image == nullptr || size == 0)
    {
        trace::panic("Can't find root image.");
    }
    global_root_file_system = memory::New<file_system>(memory::KernelCommonAllocatorV);
    vfs::register_fs(global_root_file_system);
    vfs::mount(global_root_file_system, nullptr, "/", vfs::global_root, vfs::global_root, nullptr,
               size + memory::page_size * 4);

    tar_loader(start_root_image, size);
}

file_system::file_system()
    : ramfs::file_system("rootfs")
{
}

vfs::super_block *file_system::load(const char *device_name, const byte *data, u64 size)
{
    super_block *su_block = memory::New<super_block>(memory::KernelCommonAllocatorV, this);
    su_block->load();
    return su_block;
}

void file_system::unload(vfs::super_block *su_block)
{
    su_block->save();
    memory::Delete<>(memory::KernelCommonAllocatorV, su_block);
}

int oct2bin(char *str, int size)
{
    int n = 0;
    char *c = str;
    while (--size > 0)
    {
        n *= 8;
        n += *c - '0';
        c++;
    }
    return n;
}

struct tar_header
{
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag[1];
    char linkfile[100];
    char ustar[6];
    char ustarversion[2];
    char owner_username[32];
    char owner_groupname[32];
    char device_major[8];
    char device_minor[8];
    char prefix[155];
    char pad[12];
};

char TAR_MAGIC[] = "ustar ";

int parse_single(byte *offset)
{
    auto header = reinterpret_cast<tar_header *>(offset);
    if (util::memcmp(TAR_MAGIC, header->ustar, sizeof(header->ustar)) != 0)
    {
        return -1;
    }

    int file_size = oct2bin(header->size, sizeof(header->size));
    int mode = oct2bin(header->mode, sizeof(header->mode));

    char filename[256];
    char to_filename[256];
    i64 name_offset = 0;
    if (header->prefix[0] != '\0')
    {
        name_offset = util::strcopy(filename, header->prefix, sizeof(header->prefix));
    }
    name_offset += util::strcopy(filename + name_offset, header->filename, sizeof(header->filename));

    util::strcopy(to_filename, header->linkfile, sizeof(header->linkfile));

    kassert(name_offset != 256, "filename too long");

    char tag = header->typeflag[0];
    if (tag == '\0' || tag == '0' || tag == '7')
    {
        // normal file
        auto file = fs::vfs::open(filename, fs::vfs::global_root, fs::vfs::global_root, fs::mode::write,
                                  fs::path_walk_flags::auto_create_file);
        kassert(file, "create file ", filename, " fail");
        file->write(offset + 512, file_size, 0);
        fs::vfs::chmod(filename, fs::vfs::global_root, fs::vfs::global_root, mode);
    }
    else if (tag == '1')
    {
        // hard link
        fs::vfs::link(filename, to_filename, fs::vfs::global_root, fs::vfs::global_root);
        fs::vfs::chmod(filename, fs::vfs::global_root, fs::vfs::global_root, mode);
    }
    else if (tag == '2')
    {
        // symbol link
        fs::vfs::symbolink(filename, to_filename, fs::vfs::global_root, fs::vfs::global_root, 0);
        fs::vfs::chmod(filename, fs::vfs::global_root, fs::vfs::global_root, mode);
    }
    else if (tag == '5')
    {
        // dir
        fs::vfs::mkdir(filename, fs::vfs::global_root, fs::vfs::global_root, 0);
        fs::vfs::chmod(filename, fs::vfs::global_root, fs::vfs::global_root, mode);
    }
    return (file_size + 512 + 511) & ~511;
}

void tar_loader(byte *start_root_image, u64 size)
{
    u64 offset = 0;
    while (offset < size)
    {
        int u = parse_single(start_root_image + offset);
        if (u == -1)
        {
            break;
        }
        offset += u;
    }
}

} // namespace fs::rootfs
