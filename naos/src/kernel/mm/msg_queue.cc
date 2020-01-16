#include "kernel/mm/msg_queue.hpp"

namespace memory
{
u64 lev[] = {1024, 65536};
util::id_level_generator<2> *msg_queue_id_generator = nullptr;
void init() { msg_queue_id_generator = memory::New<util::id_level_generator<2>>(memory::KernelCommonAllocatorV, lev); }

} // namespace memory
