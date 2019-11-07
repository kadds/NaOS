#include "kernel/task/builtin/init_task.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"
namespace task::builtin::init
{

void main(u64 args) { trace::info("init task running."); }
} // namespace task::builtin::init
