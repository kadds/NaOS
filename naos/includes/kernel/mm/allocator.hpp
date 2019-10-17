#pragma once
#include "common.hpp"

namespace memory
{
/// Interface of all allocators.
class IAllocator
{
  public:
    IAllocator(){};
    virtual ~IAllocator(){};
    /// Allocate a memory address.
    /// Default is virtual address.
    ///
    /// \param size The memory size which you want to allocate
    /// \param align The pointer alignment value
    /// \return Return the address has been allocate
    virtual void *allocate(u64 size, u64 align) = 0;
    /// Deallocate a memory address.
    /// Default is virtual address.
    ///
    /// \param ptr The address which you want to deallocate
    /// \return None
    virtual void deallocate(void *ptr) = 0;
};

} // namespace memory
