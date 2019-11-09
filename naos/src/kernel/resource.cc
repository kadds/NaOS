#include "kernel/resource.hpp"
#include "kernel/mm/memory.hpp"
namespace task
{
const u64 file_id_table[] = {128, 4096, 65536};
resource_table_t::resource_table_t()
    : console_attribute(nullptr)
    , file_map(memory::KernelMemoryAllocatorV)
    , id_gen(file_id_table)
{
}

file_desc resource_table_t::new_file_desc(fs::vfs::file *file)
{
    auto id = id_gen.next();
    file_map[id] = file;
    return id;
}

void resource_table_t::delete_file_desc(file_desc fd)
{
    id_gen.collect(fd);
    file_map.remove_once(fd);
    file_map[fd] = nullptr;
}

fs::vfs::file *resource_table_t::get_file(file_desc fd)
{
    fs::vfs::file *f = nullptr;
    file_map.get(fd, &f);
    return f;
}

void resource_table_t::set_file(file_desc fd, fs::vfs::file *file)
{
    if (file_map.has(fd))
    {
        file_map[fd] = file;
    }
}

trace::console_attribute *resource_table_t::get_console_attribute() { return console_attribute; }

void resource_table_t::set_console_attribute(trace::console_attribute *ca) { console_attribute = ca; }
} // namespace task
