#include "kernel/fs/vfs/standalone_file.hpp"

namespace fs::vfs
{

void standalone_file::flush() {}
i64 standalone_file::iread(i64 &offset, byte *ptr, u64 max_size, flag_t flags) { return 0; }
i64 standalone_file::iwrite(i64 &offset, const byte *ptr, u64 size, flag_t flags) { return 0; }

} // namespace fs::vfs