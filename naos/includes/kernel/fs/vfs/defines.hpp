#pragma once
#include "../../types.hpp"
#include "common.hpp"
namespace fs
{
namespace permission_flags
{
enum permission : flag_t
{
    x = 1,
    write = 2,
    read = 4,
    xr = x | read,
    wr = write | read,
    xwr = wr | x,

    group_x = 16,
    group_write = 32,
    group_read = 64,
    group_xr = group_x | group_read,
    group_wr = group_write | group_read,
    group_xwr = group_wr | group_x,

    other_x = 256,
    other_write = 512,
    other_read = 1024,
    other_xr = other_x | other_read,
    other_wr = other_write | other_read,
    other_xwr = other_wr | other_x,

    all_x = x | group_x | other_x,
    all_write = write | group_write | other_write,
    all_read = read | group_read | other_read,
    all_xr = xr | group_xr | other_xr,
    all_wr = all_write | all_read,
    all_xwr = all_wr | all_x,
};
} // namespace permission_flags

namespace access_flags
{
enum access_flags : flag_t
{
    exec = 1,
    write = 2,
    read = 4,
    exist = 8,
};
} // namespace access_flags

namespace mode
{
/// open file mode
enum mode : flag_t
{
    read = 1,
    write = 2,
    bin = 4,
    append = 8,
    no_block = 16,
    unlink_on_close = 32,
    stand_alone = 64,
};
} // namespace mode

namespace create_flags
{
enum create_flags : flag_t
{
    directory = 1,
    file = 2,
    chr = 4,
    block = 8,
    pipe = 16,
    socket = 32,
};
} // namespace create_flags

namespace path_walk_flags
{
enum path_walk_flags : flag_t
{
    auto_create_file = 1,
    continue_parse = 4,
    not_resolve_symbolic_link = 8,
    directory = 16,
    file = 32,
    cross_root = 64,
    symbolic_file = 128,
};
} // namespace path_walk_flags

enum class inode_type_t : u8
{
    file,
    directory,
    symbolink,
    socket,
    block,
    chr,
    pipe,
};

namespace symlink_flags
{
enum symlink_flags : flag_t
{
    directory = 1,

};
}

namespace vfs
{
class inode;
class dentry;
class file;
class file_system;
struct nameidata;
class super_block;
} // namespace vfs

struct dirstream
{
    file_desc fd;
};
/// file system flags
namespace fs_flags
{
enum : flag_t
{
    kernel_only = 1,

};
} // namespace fs_flags

inline constexpr u64 directory_maximum_entry_size = 512;
inline constexpr u64 directory_maximum_path_size = 4096;

struct dir_entry_str
{
    char name[directory_maximum_entry_size];
    char *operator*() { return name; }
};

struct dir_path_str
{
    char name[directory_maximum_path_size];
    char *operator*() { return name; }
};

namespace fcntl_type
{
enum
{
    set,
    get,
};
}

namespace fcntl_attr
{
enum
{
    mtime,
    ctime,
    atime,
    btime,
    uid,
    gid,
    link_count,
    inode_number,
    permission,
    type,
    pseudo_func,
};
}
/// read write flags
namespace rw_flags
{
enum
{
    no_block = 1,
    override = 2,
};
}

namespace domain
{
enum
{
    read_dir = 1,
};
}

} // namespace fs
