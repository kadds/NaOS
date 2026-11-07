#pragma once

#include "kernel/iommu/amd_vi_page_entry.hpp"

namespace iommu::amd_vi
{
enum class page_table_error : u8
{
    none,
    invalid_table,
    invalid_operations,
    invalid_address,
    invalid_permissions,
    allocation_failed,
    address_translation_failed,
    cache_maintenance_unavailable,
    already_mapped,
    not_mapped,
    table_state_uncertain,
};

struct page_table_ops
{
    bool (*allocate_page)(void *context, void *&virtual_page, u64 &physical_address) = nullptr;
    void (*release_page)(void *context, void *virtual_page) = nullptr;
    void *(*physical_to_virtual)(void *context, u64 physical_address) = nullptr;
    bool (*flush_page)(void *context, void *virtual_page) = nullptr;
    void *context = nullptr;
};

/// One AMD Mode=4, four-level, 48-bit I/O page table. The controller must
/// invalidate the AMD IOTLB before reusing an unmapped IOVA or releasing this
/// tree after its DTE has been detached.
struct page_table
{
    void *root_virtual = nullptr;
    u64 root_physical = 0;
    u8 physical_address_width = 0;
    bool coherent = false;
    bool state_uncertain = false;
    page_table_ops operations{};
};

page_table_error create_page_table(u8 physical_address_width, bool coherent, const page_table_ops &operations,
                                   page_table &table) noexcept;
page_table_error map_dma_page(page_table &table, u64 iova, u64 physical_address, dma_access access) noexcept;
page_table_error unmap_dma_page(page_table &table, u64 iova) noexcept;

/// The owner must first remove every DTE reference and complete the required
/// device-table and IOTLB invalidations.
page_table_error destroy_page_table(page_table &table) noexcept;
} // namespace iommu::amd_vi
