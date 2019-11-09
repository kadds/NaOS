#pragma once
#include "kernel/mm/new.hpp"
#include "types.hpp"
#include "util/hash_map.hpp"
#include "util/id_generator.hpp"

namespace fs::vfs
{
class file;
}
namespace trace
{
struct console_attribute;

} // namespace trace

namespace task
{

using file_map_t = util::hash_map<file_desc, fs::vfs::file *>;

struct resource_table_t
{
  private:
    trace::console_attribute *console_attribute;
    file_map_t file_map;
    util::id_level_generator<3> id_gen;

  public:
    resource_table_t();
    file_desc new_file_desc(fs::vfs::file *);
    void delete_file_desc(file_desc fd);
    fs::vfs::file *get_file(file_desc fd);
    void set_file(file_desc fd, fs::vfs::file *file);
    trace::console_attribute *get_console_attribute();
    void set_console_attribute(trace::console_attribute *ca);
};
} // namespace task
