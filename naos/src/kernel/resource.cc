#include "kernel/resource.hpp"
#include "kernel/mm/memory.hpp"
namespace task
{
const u64 file_id_table[] = {128, 4096, 65536};

file_table::file_table()
    : file_map(memory::KernelMemoryAllocatorV)
    , id_gen(file_id_table)
{
}
fs::vfs::dentry *file_table::get_path_root(const char *path)
{
    if (*path == '/')
        return root;
    return current;
}

resource_table_t::resource_table_t(file_table *ft)
    : console_attribute(nullptr)

{
    if (ft != nullptr)
    {
        f_table = ft;
    }
    else
    {
        f_table = memory::New<file_table>(memory::KernelCommonAllocatorV);
    }

    void copy_file_table(file_table * raw_ft);
}

resource_table_t::~resource_table_t() {}

void resource_table_t::copy_file_table(file_table *raw_ft)
{
    if (f_table == nullptr)
    {
        return;
    }
}

file_desc resource_table_t::new_file_desc(fs::vfs::file *file)
{
    auto id = f_table->id_gen.next();
    f_table->file_map[id] = file;
    return id;
}

void resource_table_t::delete_file_desc(file_desc fd)
{
    f_table->id_gen.collect(fd);
    f_table->file_map.remove_once(fd);
    f_table->file_map[fd] = nullptr;
}

fs::vfs::file *resource_table_t::get_file(file_desc fd)
{
    fs::vfs::file *f = nullptr;
    f_table->file_map.get(fd, &f);
    return f;
}

void resource_table_t::set_file(file_desc fd, fs::vfs::file *file)
{
    if (f_table->file_map.has(fd))
    {
        f_table->file_map[fd] = file;
    }
}

trace::console_attribute *resource_table_t::get_console_attribute() { return console_attribute; }

void resource_table_t::set_console_attribute(trace::console_attribute *ca) { console_attribute = ca; }
} // namespace task
