#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/acpi/facs_reservation.hpp"
#include "kernel/pci/msix_table_lease.hpp"
#include "kernel/physical_resource.hpp"

#include <array>
#include <cstdlib>
#include <utility>

namespace arch::idt
{
void enable() {}
void disable() {}
bool save_and_disable() { return true; }
} // namespace arch::idt

void get_task_id(u64 &, u64 &) {}

namespace
{
class host_allocator final : public freelibcxx::Allocator
{
  public:
    void *allocate(size_t size, size_t align) noexcept override
    {
        (void)align;
        return std::malloc(size);
    }

    void deallocate(void *ptr) noexcept override { std::free(ptr); }
};

template <typename... Args> bool reserve_top_level(physical_resource::ledger &ledger_value, Args &&...args)
{
    const physical_resource::root authority(&ledger_value);
    return ledger_value.reserve(authority, std::forward<Args>(args)...);
}
} // namespace

TEST_CASE("physical resource ledger rejects overlapping page envelopes", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease first{};

    REQUIRE(reserve_top_level(ledger, 0xfee0'0ffc, 8, 0xfee0'0000, 0xfee0'2000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 7, first));

    physical_resource::lease overlapping{};
    REQUIRE_FALSE(reserve_top_level(ledger, 0xfee0'1000, 4, 0xfee0'1000, 0xfee0'2000,
                                    physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                    physical_resource::access::read, 7, overlapping));
}

TEST_CASE("FACS firmware-state reservation blocks generic SystemMemory lease envelopes", "[physical_resource][acpi]")
{
    constexpr u64 page_size = 0x1000;
    constexpr u64 facs_base = 0x0020'0080;
    constexpr u64 facs_length = 64;
    constexpr u64 page_base = 0x0020'0000;
    constexpr u64 page_end = page_base + page_size;
    constexpr u64 generation = 17;

    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    const physical_resource::root authority(&ledger);
    physical_resource::lease facs{};
    REQUIRE(arch::acpi_boot::reserve_facs_state_range(authority, facs_base, facs_length, page_size, generation, facs));
    CHECK(facs.resource_kind == physical_resource::kind::firmware_state);
    CHECK(facs.mode == physical_resource::access::read);
    CHECK(facs.page_base == page_base);
    CHECK(facs.page_end == page_end);

    physical_resource::lease direct_region{};
    CHECK_FALSE(reserve_top_level(ledger, page_base + 0x800, 4, page_base, page_end,
                                  physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                  physical_resource::access::read, generation, direct_region));

    physical_resource::lease adjacent_region{};
    CHECK(reserve_top_level(ledger, page_end, 4, page_end, page_end + page_size, physical_resource::kind::device_mmio,
                            physical_resource::cache::uncacheable, physical_resource::access::read, generation,
                            adjacent_region));

    physical_resource::ledger collision_ledger(&allocator);
    const physical_resource::root collision_authority(&collision_ledger);
    physical_resource::lease conflicting_region{};
    REQUIRE(reserve_top_level(collision_ledger, page_base + 0x800, 4, page_base, page_end,
                              physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                              physical_resource::access::read, generation, conflicting_region));
    physical_resource::lease rejected_facs{};
    CHECK_FALSE(arch::acpi_boot::reserve_facs_state_range(collision_authority, facs_base, facs_length, page_size,
                                                          generation, rejected_facs));
    CHECK(rejected_facs.id == 0);
}

TEST_CASE("FACS reservation rejects invalid page geometry and overflowing envelopes", "[physical_resource][acpi]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    const physical_resource::root authority(&ledger);
    constexpr u64 generation = 19;
    physical_resource::lease result{};

    CHECK_FALSE(arch::acpi_boot::reserve_facs_state_range(authority, 0x2000, 64, 0, generation, result));
    CHECK_FALSE(arch::acpi_boot::reserve_facs_state_range(authority, 0x2000, 64, 0x1800, generation, result));
    CHECK_FALSE(arch::acpi_boot::reserve_facs_state_range(authority, 0, 64, 0x1000, generation, result));
    CHECK_FALSE(arch::acpi_boot::reserve_facs_state_range(authority, 0x2000, 0, 0x1000, generation, result));
    CHECK_FALSE(arch::acpi_boot::reserve_facs_state_range(authority, ~u64(0) - 127, 64, 0x1000, generation, result));
    CHECK(result.id == 0);
}

TEST_CASE("FACS reservation collision prevents firmware capability publication", "[physical_resource][acpi]")
{
    constexpr u64 page_size = 0x1000;
    constexpr u64 facs_length = 64;
    constexpr u64 first_page = 0x0030'0000;
    constexpr u64 conflicting_page = first_page + 2 * page_size;
    constexpr u64 first_facs_base = first_page + 0x80;
    constexpr u64 conflicting_facs_base = conflicting_page + 0x80;
    constexpr u64 generation = 23;

    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    const physical_resource::root authority(&ledger);
    physical_resource::lease existing_region{};
    REQUIRE(reserve_top_level(ledger, conflicting_page + 0x800, 4, conflicting_page, conflicting_page + page_size,
                              physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                              physical_resource::access::read, generation, existing_region));

    bool firmware_root_published = false;
    bool resource_authority_published = false;
    const bool initialized = arch::acpi_boot::reserve_facs_state_then(
        authority, page_size, generation,
        [&](auto reserve_range) {
            return reserve_range(first_facs_base, facs_length) && reserve_range(conflicting_facs_base, facs_length);
        },
        [&] {
            firmware_root_published = true;
            resource_authority_published = true;
            return true;
        });

    CHECK_FALSE(initialized);
    CHECK_FALSE(firmware_root_published);
    CHECK_FALSE(resource_authority_published);

    physical_resource::lease first_page_reused{};
    CHECK(reserve_top_level(ledger, first_page + 0x400, 4, first_page, first_page + page_size,
                            physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                            physical_resource::access::read, generation, first_page_reused));
}

TEST_CASE("FACS reservation bound rolls back before firmware capability publication", "[physical_resource][acpi]")
{
    constexpr u64 page_size = 0x1000;
    constexpr u64 facs_length = 64;
    constexpr u64 first_page = 0x0040'0000;
    constexpr u64 generation = 29;

    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    const physical_resource::root authority(&ledger);
    bool firmware_capability_published = false;
    const bool initialized = arch::acpi_boot::reserve_facs_state_then(
        authority, page_size, generation,
        [&](auto reserve_range) {
            for (size_t index = 0; index <= arch::acpi_boot::max_facs_state_ranges; ++index)
            {
                const u64 base = first_page + index * 2 * page_size + 0x80;
                if (!reserve_range(base, facs_length))
                    return false;
            }
            return true;
        },
        [&] {
            firmware_capability_published = true;
            return true;
        });

    CHECK_FALSE(initialized);
    CHECK_FALSE(firmware_capability_published);
    physical_resource::lease reused_page{};
    CHECK(reserve_top_level(ledger, first_page + 0x400, 4, first_page, first_page + page_size,
                            physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                            physical_resource::access::read, generation, reused_page));
}

TEST_CASE("physical resource ledger rejects a top-level lease without root authority", "[physical_resource][root]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease resource{};

    REQUIRE_FALSE(ledger.reserve(physical_resource::ledger::root_id(), 0xf500'0000, 0x1000, 0xf500'0000, 0xf500'1000,
                                 physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 1, resource));

    physical_resource::ledger other_ledger(&allocator);
    const physical_resource::root foreign_authority(&other_ledger);
    REQUIRE_FALSE(ledger.reserve(foreign_authority, 0xf500'0000, 0x1000, 0xf500'0000, 0xf500'1000,
                                 physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 1, resource));
    REQUIRE(reserve_top_level(ledger, 0xf500'0000, 0x1000, 0xf500'0000, 0xf500'1000,
                              physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                              physical_resource::access::read, 1, resource));
}

TEST_CASE("physical resource ledger keeps I/O and memory in separate address domains", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease physical{};
    physical_resource::lease io_port{};

    REQUIRE(reserve_top_level(ledger, 0x400, 4, 0, 0x1000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 1, physical));
    REQUIRE(reserve_top_level(ledger, 0x400, 4, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 1,
                              io_port));
    physical_resource::lease conflicting_port{};
    CHECK_FALSE(reserve_top_level(ledger, 0x402, 2, 0, 0x1000, physical_resource::kind::io_port,
                                  physical_resource::cache::uncacheable, physical_resource::access::read, 1,
                                  conflicting_port));

    physical_resource::ledger io_and_memory_ledger(&allocator);
    physical_resource::lease separate_port{};
    physical_resource::lease separate_memory{};
    REQUIRE(reserve_top_level(io_and_memory_ledger, 0x500, 4, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 1,
                              separate_port));
    REQUIRE(reserve_top_level(io_and_memory_ledger, 0x500, 4, 0, 0x1000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 1,
                              separate_memory));
}

TEST_CASE("direct I/O leases record undirected CPU access rather than fake read rights", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease port{};

    REQUIRE(reserve_top_level(ledger, 0x600, 8, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::access, 7,
                              physical_resource::sharing::exclusive, 1, port));
    REQUIRE(ledger.transition(port.id, physical_resource::state::reserved, physical_resource::state::leased, 7));
    CHECK_FALSE(ledger.covers(port.id, 0x600, 8, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read, port.generation));
    CHECK(ledger.covers(port.id, 0x600, 8, physical_resource::kind::io_port, physical_resource::cache::uncacheable,
                        physical_resource::access::access, port.generation));
    CHECK_FALSE(ledger.map(port.id, 0x600, 8, physical_resource::kind::io_port, physical_resource::cache::uncacheable,
                           physical_resource::access::access, port.generation, 7));

    physical_resource::lease fake_read_child{};
    CHECK_FALSE(ledger.reserve(port.id, 0x600, 4, 0, 0x1000, physical_resource::kind::io_port,
                               physical_resource::cache::uncacheable, physical_resource::access::read, 7,
                               physical_resource::sharing::exclusive, port.generation, fake_read_child));

    physical_resource::lease access_child{};
    CHECK(ledger.reserve(port.id, 0x600, 4, 0, 0x1000, physical_resource::kind::io_port,
                         physical_resource::cache::uncacheable, physical_resource::access::access, 7,
                         physical_resource::sharing::exclusive, port.generation, access_child));
}

TEST_CASE("mediated read-only I/O leases deny write authorization", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease port{};

    REQUIRE(reserve_top_level(ledger, 0x600, 8, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 7,
                              physical_resource::sharing::exclusive, 1, port));
    REQUIRE(ledger.bind_thread(port.id, 7, 123));
    REQUIRE(ledger.transition(port.id, physical_resource::state::reserved, physical_resource::state::leased, 7));
    CHECK(ledger.covers(port.id, 0x600, 8, physical_resource::kind::io_port, physical_resource::cache::uncacheable,
                        physical_resource::access::read, port.generation));
    CHECK_FALSE(ledger.covers(port.id, 0x600, 8, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write,
                              port.generation));
    CHECK_FALSE(ledger.map(port.id, 0x600, 8, physical_resource::kind::io_port, physical_resource::cache::uncacheable,
                           physical_resource::access::read, port.generation, 7));
}

TEST_CASE("physical resource ledger atomically admits and revokes mappings", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease resource{};
    REQUIRE(reserve_top_level(ledger, 0xfec0'0000, 0x1000, 0xfec0'0000, 0xfec0'1000, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 9, resource));
    REQUIRE(ledger.transition(resource.id, physical_resource::state::reserved, physical_resource::state::leased));

    REQUIRE_FALSE(ledger.map(resource.id, resource.base, resource.length, resource.resource_kind, resource.cache_type,
                             resource.mode, 8));
    REQUIRE(ledger.map(resource.id, resource.base + 4, 4, resource.resource_kind, resource.cache_type, resource.mode,
                       resource.generation));
    REQUIRE_FALSE(ledger.map(resource.id, resource.base, resource.length, resource.resource_kind, resource.cache_type,
                             resource.mode, resource.generation));

    physical_resource::lease mapped{};
    REQUIRE(ledger.lookup(resource.id, mapped));
    REQUIRE(mapped.lifecycle == physical_resource::state::mapped);
    REQUIRE(ledger.begin_mapping(resource.id, resource.base + 8, 8));
    REQUIRE_FALSE(ledger.revoke(resource.id));
    REQUIRE(ledger.end_mapping(resource.id));
    REQUIRE(ledger.revoke(resource.id));
    REQUIRE(ledger.revoke(resource.id));
    REQUIRE_FALSE(ledger.begin_mapping(resource.id, resource.base, resource.length));
    REQUIRE_FALSE(ledger.covers(resource.id, resource.base, resource.length, resource.resource_kind,
                                resource.cache_type, resource.mode, resource.generation));
}

TEST_CASE("direct mapping pins the complete authorized page cover", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease resource{};
    REQUIRE(reserve_top_level(ledger, 0x610ffc, 8, 0x610000, 0x612000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 19, resource));
    REQUIRE(ledger.transition(resource.id, physical_resource::state::reserved, physical_resource::state::leased));
    REQUIRE(ledger.map(resource.id, resource.base, resource.length, resource.resource_kind, resource.cache_type,
                       resource.mode, resource.generation));
    REQUIRE(ledger.begin_mapping(resource.id, resource.page_base, resource.page_end - resource.page_base));
    physical_resource::lease mapped{};
    REQUIRE(ledger.lookup(resource.id, mapped));
    REQUIRE(mapped.active_mappings == 1);
    REQUIRE(ledger.end_mapping(resource.id));
    REQUIRE(ledger.unmap_kernel(resource.id));
    REQUIRE(ledger.revoke(resource.id));
}

TEST_CASE("physical resource ledger revokes a complete lease subtree atomically", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease parent{};
    REQUIRE(reserve_top_level(ledger, 0x100000, 0x4000, 0x100000, 0x104000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 12,
                              parent));
    REQUIRE(ledger.transition(parent.id, physical_resource::state::reserved, physical_resource::state::leased));

    physical_resource::lease child{};
    REQUIRE(ledger.reserve(parent.id, 0x101000, 0x1000, 0x101000, 0x102000, physical_resource::kind::device_mmio,
                           physical_resource::cache::uncacheable, physical_resource::access::read_write, 12, child));
    REQUIRE(ledger.transition(child.id, physical_resource::state::reserved, physical_resource::state::leased));
    REQUIRE(ledger.map(parent.id, parent.base, parent.length, parent.resource_kind, parent.cache_type, parent.mode,
                       parent.generation));
    REQUIRE(ledger.map(child.id, child.base, child.length, child.resource_kind, child.cache_type, child.mode,
                       child.generation));
    REQUIRE(ledger.begin_mapping(parent.id, parent.base, parent.length));
    REQUIRE(ledger.begin_mapping(child.id, child.base, child.length));

    REQUIRE_FALSE(ledger.revoke_subtree(parent.id));
    REQUIRE(ledger.end_mapping(parent.id));
    REQUIRE(ledger.end_mapping(child.id));
    REQUIRE(ledger.revoke_subtree(parent.id));
    physical_resource::lease revoked_parent{};
    physical_resource::lease revoked_child{};
    REQUIRE_FALSE(ledger.lookup(parent.id, revoked_parent));
    REQUIRE_FALSE(ledger.lookup(child.id, revoked_child));
    REQUIRE_FALSE(ledger.covers(child.id, child.base, child.length, child.resource_kind, child.cache_type, child.mode,
                                child.generation));
    REQUIRE(ledger.revoke_subtree(parent.id));
}

TEST_CASE("physical resource subtree revoke does not partially change a reserved child", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease parent{};
    REQUIRE(reserve_top_level(ledger, 0x300000, 0x2000, 0x300000, 0x302000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 13, parent));
    REQUIRE(ledger.transition(parent.id, physical_resource::state::reserved, physical_resource::state::leased));
    REQUIRE(ledger.map(parent.id, parent.base, parent.length, parent.resource_kind, parent.cache_type, parent.mode,
                       parent.generation));

    physical_resource::lease child{};
    REQUIRE(ledger.reserve(parent.id, 0x301000, 0x1000, 0x301000, 0x302000, physical_resource::kind::device_mmio,
                           physical_resource::cache::uncacheable, physical_resource::access::read, 13, child));
    REQUIRE_FALSE(ledger.revoke_subtree(parent.id));

    physical_resource::lease unchanged_parent{};
    physical_resource::lease unchanged_child{};
    REQUIRE(ledger.lookup(parent.id, unchanged_parent));
    REQUIRE(ledger.lookup(child.id, unchanged_child));
    REQUIRE(unchanged_parent.lifecycle == physical_resource::state::mapped);
    REQUIRE(unchanged_child.lifecycle == physical_resource::state::reserved);
}

TEST_CASE("physical resource ledger rejects malformed page covers", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease resource{};

    REQUIRE_FALSE(reserve_top_level(ledger, 0x1000, 0, 0x1000, 0x2000, physical_resource::kind::device_mmio,
                                    physical_resource::cache::uncacheable, physical_resource::access::read, 1,
                                    resource));
    REQUIRE_FALSE(reserve_top_level(ledger, ~u64(0) - 3, 8, 0, 0x1000, physical_resource::kind::device_mmio,
                                    physical_resource::cache::uncacheable, physical_resource::access::read, 1,
                                    resource));

    constexpr u64 logical_base = 0x1103;
    constexpr u64 logical_length = 0x10;
    constexpr u64 exact_page_base = 0x1000;
    constexpr u64 exact_page_end = 0x2000;
    CHECK_FALSE(reserve_top_level(ledger, logical_base, logical_length, 0, exact_page_end,
                                  physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                  physical_resource::access::read, 1, resource));
    CHECK_FALSE(reserve_top_level(ledger, logical_base, logical_length, exact_page_base, exact_page_end + 0x1000,
                                  physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                  physical_resource::access::read, 1, resource));
    CHECK_FALSE(reserve_top_level(ledger, logical_base, logical_length, exact_page_base, exact_page_base,
                                  physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                  physical_resource::access::read, 1, resource));

    REQUIRE(reserve_top_level(ledger, logical_base, logical_length, exact_page_base, exact_page_end,
                              physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                              physical_resource::access::read, 1, resource));
    CHECK(resource.page_base == exact_page_base);
    CHECK(resource.page_end == exact_page_end);
}

TEST_CASE("physical resource ledger requires a live parent root", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease resource{};

    REQUIRE_FALSE(ledger.reserve(0xfeed, 0x1000, 0x1000, 0x1000, 0x2000, physical_resource::kind::device_mmio,
                                 physical_resource::cache::uncacheable, physical_resource::access::read, 1, resource));
}

TEST_CASE("physical resource ledger treats I/O ports as scalar ranges", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease first{};
    REQUIRE(reserve_top_level(ledger, 0x100, 4, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 1, first));

    physical_resource::lease adjacent{};
    REQUIRE(reserve_top_level(ledger, 0x200, 4, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 1,
                              adjacent));

    physical_resource::lease overlapping{};
    REQUIRE_FALSE(reserve_top_level(ledger, 0x102, 4, 0, 0x1000, physical_resource::kind::io_port,
                                    physical_resource::cache::uncacheable, physical_resource::access::read_write, 1,
                                    overlapping));

    physical_resource::lease high_port{};
    REQUIRE(reserve_top_level(ledger, 0x1800, 0x20, 0x1000, 0x2000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 1,
                              high_port));

    physical_resource::lease cross_page_port{};
    REQUIRE(reserve_top_level(ledger, 0x1ff8, 0x10, 0x1000, 0x3000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 1,
                              cross_page_port));
}

TEST_CASE("read-only PCI function mappings may share a root ECAM lease page cover", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease mcfg{};
    REQUIRE(reserve_top_level(ledger, 0xe000'0000, 1 << 20, 0xe000'0000, 0xe010'0000, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read,
                              physical_resource::ledger::kernel_owner(), physical_resource::sharing::shared_read_only,
                              31, mcfg));
    REQUIRE(ledger.transition(mcfg.id, physical_resource::state::reserved, physical_resource::state::leased));

    physical_resource::lease root_window{};
    REQUIRE(ledger.reserve(mcfg.id, 0xe000'0000, 1 << 20, 0xe000'0000, 0xe010'0000, physical_resource::kind::pci_ecam,
                           physical_resource::cache::uncacheable, physical_resource::access::read, 70,
                           physical_resource::sharing::shared_read_only, 31, root_window));
    REQUIRE(
        ledger.transition(root_window.id, physical_resource::state::reserved, physical_resource::state::leased, 70));
    REQUIRE(ledger.map(root_window.id, root_window.base, root_window.length, root_window.resource_kind,
                       root_window.cache_type, root_window.mode, root_window.generation, 70));

    physical_resource::lease function_page{};
    REQUIRE(ledger.reserve(mcfg.id, 0xe000'8000, 0x1000, 0xe000'8000, 0xe000'9000, physical_resource::kind::pci_ecam,
                           physical_resource::cache::uncacheable, physical_resource::access::read, 80,
                           physical_resource::sharing::shared_read_only, 31, function_page));
    CHECK(function_page.owner == 80);
}

TEST_CASE("direct leases cannot share a page already exposed by another mapping", "[physical_resource]")
{
    host_allocator first_allocator;
    physical_resource::ledger first_ledger(&first_allocator);
    physical_resource::lease direct{};
    REQUIRE(reserve_top_level(first_ledger, 0x700000, 0x100, 0x700000, 0x701000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 41,
                              physical_resource::sharing::exclusive, 21, direct));
    REQUIRE(
        first_ledger.transition(direct.id, physical_resource::state::reserved, physical_resource::state::leased, 41));
    REQUIRE(first_ledger.map(direct.id, direct.base, direct.length, direct.resource_kind, direct.cache_type,
                             direct.mode, direct.generation, 41));
    REQUIRE(first_ledger.begin_mapping(direct.id, direct.page_base, direct.page_end - direct.page_base));

    host_allocator second_allocator;
    physical_resource::ledger second_ledger(&second_allocator);
    physical_resource::lease another_direct{};
    REQUIRE(reserve_top_level(second_ledger, 0x710800, 4, 0x710000, 0x711000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 42,
                              physical_resource::sharing::exclusive, 22, another_direct));
    physical_resource::lease direct_after_direct{};
    REQUIRE_FALSE(reserve_top_level(second_ledger, 0x710000, 0x100, 0x710000, 0x711000,
                                    physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                    physical_resource::access::read, 41, physical_resource::sharing::exclusive, 22,
                                    direct_after_direct));
}

TEST_CASE("physical resource ledger enforces owner on map and revoke", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease resource{};
    REQUIRE(reserve_top_level(ledger, 0x500000, 0x1000, 0x500000, 0x501000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 41,
                              physical_resource::sharing::exclusive, 15, resource));
    REQUIRE(resource.owner == 41);
    REQUIRE_FALSE(ledger.bind_namespace(resource.id, 42, 99));
    REQUIRE(ledger.bind_namespace(resource.id, 41, 99));
    physical_resource::lease namespaced{};
    REQUIRE(ledger.lookup(resource.id, namespaced));
    REQUIRE(namespaced.namespace_generation == 99);
    REQUIRE_FALSE(
        ledger.transition(resource.id, physical_resource::state::reserved, physical_resource::state::leased, 42));
    REQUIRE(ledger.transition(resource.id, physical_resource::state::reserved, physical_resource::state::leased, 41));
    REQUIRE_FALSE(ledger.map(resource.id, resource.base, resource.length, resource.resource_kind, resource.cache_type,
                             resource.mode, resource.generation, 42));
    REQUIRE(ledger.map(resource.id, resource.base, resource.length, resource.resource_kind, resource.cache_type,
                       resource.mode, resource.generation, 41));
    REQUIRE(ledger.begin_mapping(resource.id, resource.base, resource.length));
    REQUIRE_FALSE(ledger.revoke(resource.id, 42));
    REQUIRE_FALSE(ledger.revoke(resource.id, 41));
    REQUIRE(ledger.end_mapping(resource.id));
    REQUIRE(ledger.revoke(resource.id, 41));

    physical_resource::lease unreached_mapping{};
    REQUIRE(reserve_top_level(ledger, 0x510000, 0x1000, 0x510000, 0x511000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 43,
                              physical_resource::sharing::exclusive, 15, unreached_mapping));
    REQUIRE(ledger.transition(unreached_mapping.id, physical_resource::state::reserved,
                              physical_resource::state::leased, 43));
    REQUIRE(ledger.revoke(unreached_mapping.id, 43));
}

TEST_CASE("owner cleanup revokes unmapped leases while preserving live mappings", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease mapped{};
    physical_resource::lease leased{};
    REQUIRE(reserve_top_level(ledger, 0x520000, 0x1000, 0x520000, 0x521000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 44,
                              physical_resource::sharing::exclusive, 17, mapped));
    REQUIRE(reserve_top_level(ledger, 0x530000, 0x1000, 0x530000, 0x531000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 44,
                              physical_resource::sharing::exclusive, 17, leased));
    REQUIRE(ledger.transition(mapped.id, physical_resource::state::reserved, physical_resource::state::leased, 44));
    REQUIRE(ledger.transition(leased.id, physical_resource::state::reserved, physical_resource::state::leased, 44));
    REQUIRE(ledger.map(mapped.id, mapped.base, mapped.length, mapped.resource_kind, mapped.cache_type, mapped.mode,
                       mapped.generation, 44));
    REQUIRE(ledger.begin_mapping(mapped.id, mapped.base, mapped.length));

    REQUIRE(ledger.revoke_owner(44));
    physical_resource::lease still_mapped{};
    physical_resource::lease revoked_lease{};
    REQUIRE(ledger.lookup(mapped.id, still_mapped));
    REQUIRE_FALSE(ledger.lookup(leased.id, revoked_lease));
    REQUIRE(still_mapped.lifecycle == physical_resource::state::revoking);
    REQUIRE(still_mapped.active_mappings == 1);
    REQUIRE_FALSE(ledger.revoke_kernel(mapped.id));
    REQUIRE_FALSE(ledger.begin_mapping(mapped.id, mapped.base, mapped.length));
    REQUIRE(ledger.end_mapping(mapped.id));
    REQUIRE_FALSE(ledger.lookup(mapped.id, still_mapped));
    REQUIRE(ledger.revoke_kernel(mapped.id));
}

TEST_CASE("physical resource ledger permits only exact shared read aliases", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease first{};
    physical_resource::lease second{};
    REQUIRE(reserve_top_level(ledger, 0x600000, 0x100, 0x600000, 0x601000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 51,
                              physical_resource::sharing::shared_read_only, 16, first));
    REQUIRE(reserve_top_level(ledger, 0x600000, 0x100, 0x600000, 0x601000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 52,
                              physical_resource::sharing::shared_read_only, 16, second));

    physical_resource::lease different_length{};
    REQUIRE_FALSE(reserve_top_level(ledger, 0x600000, 0x80, 0x600000, 0x601000, physical_resource::kind::device_mmio,
                                    physical_resource::cache::uncacheable, physical_resource::access::read, 53,
                                    physical_resource::sharing::shared_read_only, 16, different_length));
    REQUIRE_FALSE(reserve_top_level(ledger, 0x600000, 0x100, 0x600000, 0x601000, physical_resource::kind::device_mmio,
                                    physical_resource::cache::uncacheable, physical_resource::access::read_write, 54,
                                    physical_resource::sharing::exclusive, 16, different_length));
}

TEST_CASE("physical resource ledger binds I/O leases to one thread", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease resource{};
    REQUIRE(reserve_top_level(ledger, 0x3f8, 8, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 61,
                              physical_resource::sharing::exclusive, 17, resource));
    REQUIRE(ledger.transition(resource.id, physical_resource::state::reserved, physical_resource::state::leased, 61));
    REQUIRE_FALSE(ledger.bind_thread(resource.id, 62, 7));
    REQUIRE_FALSE(ledger.bind_thread(resource.id, 61, 0));
    REQUIRE(ledger.bind_thread(resource.id, 61, 7));
    physical_resource::lease bound{};
    REQUIRE(ledger.lookup(resource.id, bound));
    REQUIRE(bound.thread_binding == 7);
}

TEST_CASE("thread exit revokes only that thread's I/O leases and permits reauthorization", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease exiting_thread_lease{};
    physical_resource::lease sibling_lease{};
    physical_resource::lease other_owner_lease{};

    REQUIRE(reserve_top_level(ledger, 0x3f8, 8, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 61,
                              physical_resource::sharing::exclusive, 17, exiting_thread_lease));
    REQUIRE(ledger.transition(exiting_thread_lease.id, physical_resource::state::reserved,
                              physical_resource::state::leased, 61));
    REQUIRE(ledger.bind_thread(exiting_thread_lease.id, 61, 7));

    REQUIRE(reserve_top_level(ledger, 0x2f8, 8, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 61,
                              physical_resource::sharing::exclusive, 17, sibling_lease));
    REQUIRE(
        ledger.transition(sibling_lease.id, physical_resource::state::reserved, physical_resource::state::leased, 61));
    REQUIRE(ledger.bind_thread(sibling_lease.id, 61, 8));

    REQUIRE(reserve_top_level(ledger, 0x278, 8, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 62,
                              physical_resource::sharing::exclusive, 17, other_owner_lease));
    REQUIRE(ledger.transition(other_owner_lease.id, physical_resource::state::reserved,
                              physical_resource::state::leased, 62));
    REQUIRE(ledger.bind_thread(other_owner_lease.id, 62, 7));

    REQUIRE(ledger.revoke_thread(61, 7));

    physical_resource::lease revoked{};
    physical_resource::lease still_live{};
    REQUIRE_FALSE(ledger.lookup(exiting_thread_lease.id, revoked));
    REQUIRE(ledger.lookup(sibling_lease.id, still_live));
    CHECK(still_live.lifecycle == physical_resource::state::leased);
    CHECK(ledger.revoke_thread(62, 7));

    physical_resource::lease replacement{};
    REQUIRE(reserve_top_level(ledger, 0x3f8, 8, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 61,
                              physical_resource::sharing::exclusive, 17, replacement));
    REQUIRE(
        ledger.transition(replacement.id, physical_resource::state::reserved, physical_resource::state::leased, 61));
    REQUIRE(ledger.bind_thread(replacement.id, 61, 7));
}

TEST_CASE("child lease inherits and cannot change the parent's I/O thread binding", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease parent{};
    REQUIRE(reserve_top_level(ledger, 0x3f8, 8, 0, 0x1000, physical_resource::kind::io_port,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write, 91,
                              physical_resource::sharing::exclusive, 33, parent));
    REQUIRE(ledger.transition(parent.id, physical_resource::state::reserved, physical_resource::state::leased, 91));
    REQUIRE(ledger.bind_thread(parent.id, 91, 77));

    physical_resource::lease child{};
    REQUIRE(ledger.reserve(parent.id, 0x3fa, 2, 0, 0x1000, physical_resource::kind::io_port,
                           physical_resource::cache::uncacheable, physical_resource::access::read_write, 91,
                           physical_resource::sharing::exclusive, 33, child));
    physical_resource::lease inherited{};
    REQUIRE(ledger.lookup(child.id, inherited));
    CHECK(inherited.thread_binding == 77);
    REQUIRE_FALSE(ledger.bind_thread(child.id, 91, 78));
}

TEST_CASE("physical resource ledger binds leases to an owner device", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease resource{};
    REQUIRE(reserve_top_level(ledger, 0x700000, 0x1000, 0x700000, 0x701000, physical_resource::kind::device_mmio,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 71,
                              physical_resource::sharing::exclusive, 18, resource));
    REQUIRE_FALSE(ledger.bind_device(resource.id, 72, 0x100));
    REQUIRE(ledger.bind_device(resource.id, 71, 0x100));
    REQUIRE_FALSE(ledger.bind_device(resource.id, 71, 0x200));
    physical_resource::lease bound{};
    REQUIRE(ledger.lookup(resource.id, bound));
    REQUIRE(bound.device_binding == 0x100);
}

TEST_CASE("physical resource parents scope contained child leases", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease parent{};
    REQUIRE(reserve_top_level(ledger, 0xe0000000, 0x4000, 0xe0000000, 0xe0004000, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 81,
                              physical_resource::sharing::exclusive, 29, parent));
    physical_resource::lease reserved_child{};
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe0000000, 0x1000, 0xe0000000, 0xe0001000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 81, physical_resource::sharing::exclusive, 29,
                                 reserved_child));
    REQUIRE(ledger.transition(parent.id, physical_resource::state::reserved, physical_resource::state::leased, 81));
    REQUIRE(ledger.bind_namespace(parent.id, 81, 93));
    REQUIRE(ledger.bind_device(parent.id, 81, 0x1234));

    physical_resource::lease first_child{};
    REQUIRE(ledger.reserve(parent.id, 0xe0001000, 0x1000, 0xe0001000, 0xe0002000, physical_resource::kind::pci_ecam,
                           physical_resource::cache::uncacheable, physical_resource::access::read, 81,
                           physical_resource::sharing::exclusive, 29, first_child));
    physical_resource::lease inherited_scope{};
    REQUIRE(ledger.lookup(first_child.id, inherited_scope));
    CHECK(inherited_scope.namespace_generation == 93);
    CHECK(inherited_scope.device_binding == 0x1234);
    REQUIRE_FALSE(ledger.bind_namespace(first_child.id, 81, 94));
    REQUIRE_FALSE(ledger.bind_device(first_child.id, 81, 0x5678));
    REQUIRE(
        ledger.transition(first_child.id, physical_resource::state::reserved, physical_resource::state::leased, 81));

    physical_resource::lease grandchild{};
    REQUIRE(ledger.reserve(first_child.id, 0xe0001100, 0x100, 0xe0001000, 0xe0002000, physical_resource::kind::pci_ecam,
                           physical_resource::cache::uncacheable, physical_resource::access::read, 81,
                           physical_resource::sharing::exclusive, 29, grandchild));

    physical_resource::lease adjacent_child{};
    REQUIRE(ledger.reserve(parent.id, 0xe0002000, 0x1000, 0xe0002000, 0xe0003000, physical_resource::kind::pci_ecam,
                           physical_resource::cache::uncacheable, physical_resource::access::read, 81,
                           physical_resource::sharing::exclusive, 29, adjacent_child));

    physical_resource::lease overlapping_sibling{};
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe0001800, 0x1000, 0xe0001000, 0xe0003000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 81, physical_resource::sharing::exclusive, 29,
                                 overlapping_sibling));

    physical_resource::lease revoking_parent{};
    REQUIRE(reserve_top_level(ledger, 0xe0100000, 0x1000, 0xe0100000, 0xe0101000, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 83,
                              physical_resource::sharing::exclusive, 29, revoking_parent));
    REQUIRE(ledger.transition(revoking_parent.id, physical_resource::state::reserved, physical_resource::state::leased,
                              83));
    REQUIRE(ledger.map(revoking_parent.id, revoking_parent.base, revoking_parent.length, revoking_parent.resource_kind,
                       revoking_parent.cache_type, revoking_parent.mode, revoking_parent.generation, 83));
    REQUIRE(ledger.begin_mapping(revoking_parent.id, revoking_parent.base, revoking_parent.length));
    REQUIRE(ledger.revoke_owner(83));
    physical_resource::lease denied_child{};
    REQUIRE_FALSE(ledger.reserve(revoking_parent.id, 0xe0100000, 0x1000, 0xe0100000, 0xe0101000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 83, physical_resource::sharing::exclusive, 29,
                                 denied_child));
    REQUIRE(ledger.end_mapping(revoking_parent.id));
}

TEST_CASE("physical resource child leases cannot escape or amplify the parent", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease parent{};
    REQUIRE(reserve_top_level(ledger, 0xe1000000, 0x2000, 0xe1000000, 0xe1002000, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 82,
                              physical_resource::sharing::exclusive, 30, parent));
    REQUIRE(ledger.transition(parent.id, physical_resource::state::reserved, physical_resource::state::leased, 82));

    physical_resource::lease rejected{};
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe1001000, 0x2000, 0xe1001000, 0xe1003000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 82, physical_resource::sharing::exclusive, 30,
                                 rejected));
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe1000000, 0x1000, 0xe1000000, 0xe1001000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read_write, 82, physical_resource::sharing::exclusive, 30,
                                 rejected));
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe1000000, 0x1000, 0xe1000000, 0xe1001000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::write_back,
                                 physical_resource::access::read, 82, physical_resource::sharing::exclusive, 30,
                                 rejected));
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe1000000, 0x1000, 0xe1000000, 0xe1001000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 82, physical_resource::sharing::exclusive, 31,
                                 rejected));
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe1000000, 0x1000, 0xe1000000, 0xe1001000,
                                 physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 82, physical_resource::sharing::exclusive, 30,
                                 rejected));
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe1000000, 0x1000, 0xe1000000, 0xe1001000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 84, physical_resource::sharing::exclusive, 30,
                                 rejected));
    REQUIRE_FALSE(ledger.reserve(parent.id, 0xe1000000, 0x1000, 0xe1000000, 0xe1001000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 82, physical_resource::sharing::shared_read_only, 30,
                                 rejected));

    physical_resource::lease unaligned_parent{};
    REQUIRE(reserve_top_level(ledger, 0xe1200100, 0x800, 0xe1200000, 0xe1201000, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read, 85,
                              physical_resource::sharing::exclusive, 32, unaligned_parent));
    REQUIRE(ledger.transition(unaligned_parent.id, physical_resource::state::reserved, physical_resource::state::leased,
                              85));
    REQUIRE_FALSE(ledger.reserve(unaligned_parent.id, 0xe12000f0, 0x100, 0xe1200000, 0xe1201000,
                                 physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                                 physical_resource::access::read, 85, physical_resource::sharing::exclusive, 32,
                                 rejected));
}

TEST_CASE("kernel PCI ECAM authority attenuates to user read-only mappings", "[physical_resource][pci]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::lease kernel_ecam{};
    REQUIRE(reserve_top_level(ledger, 0xe2000000, 0x200000, 0xe2000000, 0xe2200000, physical_resource::kind::pci_ecam,
                              physical_resource::cache::uncacheable, physical_resource::access::read_write,
                              physical_resource::ledger::kernel_owner(), physical_resource::sharing::exclusive, 44,
                              kernel_ecam));
    REQUIRE(ledger.transition(kernel_ecam.id, physical_resource::state::reserved, physical_resource::state::leased));

    physical_resource::lease kernel_quiesce_mapping{};
    REQUIRE(ledger.reserve(kernel_ecam.id, 0xe2000000, 0x200000, 0xe2000000, 0xe2200000,
                           physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                           physical_resource::access::read_write, physical_resource::ledger::kernel_owner(),
                           physical_resource::sharing::exclusive, 44, kernel_quiesce_mapping));
    REQUIRE(ledger.transition(kernel_quiesce_mapping.id, physical_resource::state::reserved,
                              physical_resource::state::leased));
    REQUIRE(ledger.map(kernel_quiesce_mapping.id, 0xe2000000, 0x200000, physical_resource::kind::pci_ecam,
                       physical_resource::cache::uncacheable, physical_resource::access::read_write, 44));
    REQUIRE(ledger.unmap_kernel(kernel_quiesce_mapping.id));
    physical_resource::lease released_quiesce_mapping{};
    CHECK_FALSE(ledger.lookup(kernel_quiesce_mapping.id, released_quiesce_mapping));

    physical_resource::lease user_read_only{};
    REQUIRE(ledger.reserve(kernel_ecam.id, 0xe2000000, 0x100000, 0xe2000000, 0xe2100000,
                           physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                           physical_resource::access::read, 91, physical_resource::sharing::shared_read_only, 44,
                           user_read_only));

    physical_resource::lease user_writable{};
    CHECK_FALSE(ledger.reserve(kernel_ecam.id, 0xe2100000, 0x100000, 0xe2100000, 0xe2200000,
                               physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                               physical_resource::access::read_write, 91, physical_resource::sharing::exclusive, 44,
                               user_writable));
    CHECK_FALSE(ledger.reserve(kernel_ecam.id, 0xe2100000, 0x100000, 0xe2100000, 0xe2200000,
                               physical_resource::kind::pci_ecam, physical_resource::cache::uncacheable,
                               physical_resource::access::read, 91, physical_resource::sharing::exclusive, 44,
                               user_writable));
}

TEST_CASE("kernel MSI-X table lease maps without user namespace or device bindings", "[physical_resource][pci][msix]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::root authority(&ledger);
    pci::msix_table_mapping_plan plan{};
    plan.table = {0xfed0'5000, 0x100};
    plan.page_cover = {0xfed0'5000, 0xfed0'6000};

    physical_resource::lease lease{};
    REQUIRE(pci::reserve_and_map_kernel_msix_table(ledger, authority, plan, 44, lease) ==
            pci::msix_table_lease_result::success);
    CHECK(lease.owner == physical_resource::ledger::kernel_owner());
    CHECK(lease.namespace_generation == 0);
    CHECK(lease.device_binding == 0);
    CHECK(lease.lifecycle == physical_resource::state::mapped);
    REQUIRE(ledger.unmap_kernel(lease.id));
    REQUIRE(ledger.revoke_kernel(lease.id));
}

TEST_CASE("kernel MSI-X table lease rejects a root from another resource ledger", "[physical_resource][pci][msix]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    physical_resource::ledger foreign_ledger(&allocator);
    physical_resource::root foreign_authority(&foreign_ledger);
    pci::msix_table_mapping_plan plan{};
    plan.table = {0xfed0'5000, 0x100};
    plan.page_cover = {0xfed0'5000, 0xfed0'6000};

    physical_resource::lease lease{};
    CHECK(pci::reserve_and_map_kernel_msix_table(ledger, foreign_authority, plan, 44, lease) ==
          pci::msix_table_lease_result::resource_unavailable);
    CHECK(lease.id == 0);
}

TEST_CASE("physical resource ledger enforces a fixed admission cap and reclaims revoked entries", "[physical_resource]")
{
    host_allocator allocator;
    physical_resource::ledger ledger(&allocator);
    constexpr auto capacity = physical_resource::ledger::max_lease_count();
    std::array<physical_resource::lease, capacity> leases{};

    for (std::size_t index = 0; index < capacity; ++index)
    {
        const u64 base = 0x10000000 + static_cast<u64>(index) * 0x2000;
        REQUIRE(reserve_top_level(ledger, base, 0x1000, base, base + 0x1000, physical_resource::kind::device_mmio,
                                  physical_resource::cache::uncacheable, physical_resource::access::read, 1,
                                  leases[index]));
    }

    physical_resource::lease overflow{};
    const u64 overflow_base = 0x10000000 + static_cast<u64>(capacity) * 0x2000;
    REQUIRE_FALSE(reserve_top_level(ledger, overflow_base, 0x1000, overflow_base, overflow_base + 0x1000,
                                    physical_resource::kind::device_mmio, physical_resource::cache::uncacheable,
                                    physical_resource::access::read, 1, overflow));

    for (const auto &lease : leases)
        REQUIRE(ledger.revoke(lease.id));

    REQUIRE(reserve_top_level(ledger, leases[0].base, leases[0].length, leases[0].page_base, leases[0].page_end,
                              leases[0].resource_kind, leases[0].cache_type, leases[0].mode, leases[0].generation,
                              overflow));
}
