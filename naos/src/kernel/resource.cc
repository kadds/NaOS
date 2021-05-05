#include "kernel/resource.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/mm/memory.hpp"
namespace task
{
const u64 file_id_table[] = {128, 4096, 65536};

file_table_t::file_table_t()
    : file_map(memory::KernelVirtualAllocatorV)
    , id_gen(file_id_table)
{
}

resource_table_t::resource_table_t(file_table_t *ft)
{
    if (ft != nullptr)
    {
        f_table = ft;
    }
    else
    {
        f_table = memory::New<file_table_t>(memory::KernelCommonAllocatorV);
    }
}

resource_table_t::~resource_table_t() {}

void resource_table_t::copy_file_table(file_table_t *raw_ft)
{
    if (f_table == nullptr)
    {
        return;
    }
}

file_desc resource_table_t::new_file_desc(fs::vfs::file *file)
{
    uctx::RawWriteLockUninterruptibleContext icu(f_table->filemap_lock);
    auto id = f_table->id_gen.next();
    if (id != util::null_id)
        f_table->file_map.insert(id, file);
    return id;
}

void resource_table_t::delete_file_desc(file_desc fd)
{
    uctx::RawWriteLockUninterruptibleContext icu(f_table->filemap_lock);
    f_table->id_gen.collect(fd);
    f_table->file_map.remove(fd);
}

fs::vfs::file *resource_table_t::get_file(file_desc fd)
{
    fs::vfs::file *f = nullptr;
    uctx::RawReadLockUninterruptibleContext icu(f_table->filemap_lock);
    f_table->file_map.get(fd, &f);
    return f;
}

bool resource_table_t::set_file(file_desc fd, fs::vfs::file *file)
{
    uctx::RawWriteLockUninterruptibleContext icu(f_table->filemap_lock);

    if (!f_table->file_map.has(fd))
    {
        f_table->file_map.insert(fd, file);
        return true;
    }
    return false;
}

void resource_table_t::clear()
{
    while (f_table->file_map.size() != 0)
    {
        fs::vfs::file *file = nullptr;
        {

            uctx::RawWriteLockUninterruptibleContext icu(f_table->filemap_lock);

            auto it = f_table->file_map.begin();
            if (it == f_table->file_map.end())
                break;
            file = it->value;
            f_table->file_map.remove(it->key);
        }
        if (file != nullptr)
            file->close();
    }
}

} // namespace task
