#include "kernel/init_task.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/syscall.hpp"
#include "kernel/task.hpp"
#include "kernel/trace.hpp"

namespace task::builtin::init
{

void main(u64 args)
{
    trace::info("init task running.");
    exec_segment exec;
    using namespace fs::vfs;
    file *f = open("/bin/init", mode::bin | mode::read, 0);
    u64 len = size(f);
    byte *userland_code = (byte *)memory::KernelBuddyAllocatorV->allocate(len, 8);

    read(f, userland_code, len);

    exec.start_offset = memory::kernel_virtaddr_to_phyaddr((u64)userland_code);
    exec.length = len;

    task::do_exec(exec, 0, 0);
    for (;;)
        ;
}
} // namespace task::builtin::init
