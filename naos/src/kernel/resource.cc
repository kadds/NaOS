#include "kernel/resource.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/handle.hpp"
#include "kernel/kobject.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/ucontext.hpp"
namespace task
{
const u64 id_table[] = {128, 4096, 65536};

resource_table_t::resource_table_t()
    : handle_map(memory::KernelCommonAllocatorV)
    , id_gen(id_table)
{
}

resource_table_t::~resource_table_t() {}

void resource_table_t::clone(resource_table_t *to, filter_func filter, u64 userdata)
{
    uctx::RawWriteLockUninterruptibleContext icu0(to->map_lock);
    uctx::RawWriteLockUninterruptibleContext icu(map_lock);

    for (auto it : handle_map)
    {
        if (filter(it.key, it.value, userdata))
        {
            to->id_gen.tag(it.key);
            to->handle_map.insert(it.key, it.value);
        }
    }
}

file_desc resource_table_t::new_kobject(khandle obj)
{
    uctx::RawWriteLockUninterruptibleContext icu(map_lock);
    auto id = id_gen.next();
    if (id != util::null_id)
        handle_map.insert(id, std::move(obj));
    return id;
}

void resource_table_t::delete_kobject(file_desc fd)
{
    uctx::RawWriteLockUninterruptibleContext icu(map_lock);
    id_gen.collect(fd);
    handle_map.remove(fd);
}

khandle resource_table_t::get_kobject(file_desc fd)
{
    uctx::RawWriteLockUninterruptibleContext icu(map_lock);
    return handle_map.get(fd).value_or(khandle());
}

bool resource_table_t::set_handle(file_desc fd, khandle obj)
{
    uctx::RawWriteLockUninterruptibleContext icu(map_lock);

    if (!handle_map.has(fd))
    {
        handle_map.insert(fd, std::move(obj));
        return true;
    }
    return false;
}

void resource_table_t::clear()
{
    uctx::RawWriteLockUninterruptibleContext icu(map_lock);
    handle_map.clear();
}

} // namespace task
