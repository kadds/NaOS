#pragma once

namespace task
{
struct process_t;

// The process on whose behalf a kernel operation is being authorized. This is
// an explicit, non-owning context; it does not change the executing thread or
// its resource table.
struct access_context
{
    process_t *caller = nullptr;
};
} // namespace task
