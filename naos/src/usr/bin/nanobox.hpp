#pragma once
#include "freelibcxx/allocator.hpp"
#include <stdlib.h>

class DefaultAllocator : public freelibcxx::Allocator
{
    void *allocate(size_t size, size_t align) noexcept override { return aligned_alloc(align, size); }
    void deallocate(void *ptr) noexcept override { free(ptr); }
};

extern DefaultAllocator alloc;
