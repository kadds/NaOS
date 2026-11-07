#include <catch2/catch_test_macros.hpp>

#include "kernel/mm/data_plane.hpp"

TEST_CASE("device MMIO memory objects are shared and map-only", "[memory_object][mmio]")
{
    constexpr u32 mmio = NA_MEMORY_FLAG_DEVICE_MMIO;
    constexpr u32 read = NA_MEMORY_MAP_READ;
    constexpr u32 write = NA_MEMORY_MAP_WRITE;
    constexpr u32 shared = NA_MEMORY_MAP_SHARED;
    constexpr u32 execute = NA_MEMORY_MAP_EXEC;

    CHECK(naos::data_plane::memory_object_device_mmio_mapping_is_allowed(0, read));
    CHECK(naos::data_plane::memory_object_device_mmio_mapping_is_allowed(mmio, read | shared));
    CHECK(naos::data_plane::memory_object_device_mmio_mapping_is_allowed(mmio, read | write | shared));
    CHECK_FALSE(naos::data_plane::memory_object_device_mmio_mapping_is_allowed(mmio, read));
    CHECK_FALSE(naos::data_plane::memory_object_device_mmio_mapping_is_allowed(mmio, read | shared | execute));

    CHECK(naos::data_plane::memory_object_scalar_ipc_is_allowed(0));
    CHECK_FALSE(naos::data_plane::memory_object_scalar_ipc_is_allowed(mmio));
}
