#include "kernel/init_task.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
namespace task::builtin::init
{

void main(u64 args)
{
    trace::info("init task running.");
    auto file = fs::vfs::open("/bin/init", fs::vfs::mode::read | fs::vfs::mode::bin, 0);
    task::do_exec(file, 0, 0, task::exec_flags::immediately);
    trace::panic("Can't execute /bin/int.");
}
} // namespace task::builtin::init
