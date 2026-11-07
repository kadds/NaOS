#include <cstddef>
#include <cstdint>
#include <cstring>

#include "catch2_compat.hpp"
#include "kernel/firmware_version.hpp"
#include <naos/generated/internal_uapi.h>
#include <naos/generated/system/AcpiFixedHardware.hpp>
#include <naos/generated/system/AcpiResourceAuthority.hpp>
#include <naos/generated/system/BlockDevice.hpp>
#include <naos/generated/system/BlockDeviceFactory.hpp>
#include <naos/generated/system/Directory.hpp>
#include <naos/generated/system/File.hpp>
#include <naos/generated/system/FirmwareRoot.hpp>
#include <naos/generated/system/Framebuffer.hpp>
#include <naos/generated/system/InputEventSource.hpp>
#include <naos/generated/system/InterruptSource.hpp>
#include <naos/generated/system/MemoryObject.hpp>
#include <naos/generated/system/MountTicket.hpp>
#include <naos/generated/system/PciDeviceResourceAttestation.hpp>
#include <naos/generated/system/PciDiscoveryFabric.hpp>
#include <naos/generated/system/PciDmaDomain.hpp>
#include <naos/generated/system/PciDmaFaultSource.hpp>
#include <naos/generated/system/PciFunction.hpp>
#include <naos/generated/system/PciRootResourceAttestation.hpp>
#include <naos/generated/system/PlatformPciTopology.hpp>
#include <naos/generated/system/PlatformPowerControl.hpp>
#include <naos/generated/system/Process.hpp>
#include <naos/generated/system/ServiceDirectory.hpp>
#include <naos/generated/system/Stream.hpp>
#include <naos/generated/system/SystemStatus.hpp>
#include <naos/generated/system/TerminalDriverControl.hpp>
#include <naos/generated/system/TerminalDriverFactory.hpp>
#include <naos/generated/system/TerminalJobControl.hpp>
#include <naos/generated/system/TerminalManager.hpp>
#include <naos/generated/system/TerminalMaster.hpp>
#include <naos/generated/system/TerminalSlave.hpp>
#include <naos/generated/system/Vfs.hpp>
#include <naos/generated/system_uapi.h>

template <typename T>
concept has_legacy_job_control_process_id = requires(T value) { value.process_id; };

static_assert(NA_SCOPE_STREAM == 1);
static_assert(NA_SCOPE_FILE == 2);
static_assert(NA_SCOPE_DIRECTORY == 3);
static_assert(NA_SCOPE_MEMORY_OBJECT == 7);
static_assert(NA_SCOPE_PROCESS == 9);
static_assert(NA_SCOPE_TERMINAL_MANAGER == 5);
static_assert(NA_SCOPE_TERMINAL_MASTER == 6);
static_assert(NA_SCOPE_VFS == 16);
static_assert(NA_SCOPE_BLOCK_DEVICE == 17);
static_assert(NA_SCOPE_NAMESPACE_BINDING == 18);
static_assert(NA_SCOPE_MOUNT_TICKET == 19);
static_assert(NA_SCOPE_BLOCK_DEVICE_FACTORY == 20);
static_assert(NA_SCOPE_MOUNT_CONTROL == 21);
static_assert(NA_SCOPE_MUTATION_TICKET == 22);
static_assert(NA_SCOPE_FRAMEBUFFER == 23);
static_assert(NA_SCOPE_TERMINAL_SLAVE == 11);
static_assert(NA_SCOPE_TERMINAL_JOB_CONTROL == 12);
static_assert(NA_SCOPE_TERMINAL_DRIVER_CONTROL == 13);
static_assert(NA_SCOPE_TERMINAL_DRIVER_FACTORY == 14);
static_assert(NA_SCOPE_INPUT_EVENT_SOURCE == 15);
static_assert(NA_SCOPE_INTERRUPT_SOURCE == 38);
static_assert(NA_METHOD_STREAM_READ == 1);
static_assert(NA_METHOD_STREAM_WRITE == 2);
static_assert(NA_METHOD_DIRECTORY_OPEN == 1);
static_assert(NA_METHOD_DIRECTORY_CLONE_BINDING == 14);
static_assert(NA_METHOD_DIRECTORY_STAT_NODE == 15);
static_assert(NA_METHOD_DIRECTORY_SYNC == 16);
static_assert(NA_METHOD_DIRECTORY_RENAME_AT == 17);
static_assert(NA_METHOD_DIRECTORY_LINK_AT == 18);
// Reserved ordinal persisted in the manifest; never reuse method ID 19.
static_assert(NA_METHOD_DIRECTORY_SET_METADATA == 19);
static_assert(NA_METHOD_DIRECTORY_LIST == 2);
static_assert(NA_METHOD_DIRECTORY_STAT == 3);
static_assert(NA_METHOD_DIRECTORY_CREATE == 4);
static_assert(NA_METHOD_DIRECTORY_REMOVE == 5);
static_assert(NA_METHOD_DIRECTORY_PATH == 6);
static_assert(NA_METHOD_DIRECTORY_ACCESS == 7);
static_assert(NA_METHOD_DIRECTORY_RENAME == 8);
static_assert(NA_METHOD_DIRECTORY_LINK == 9);
static_assert(NA_METHOD_DIRECTORY_SYMLINK == 10);
static_assert(NA_METHOD_DIRECTORY_READLINK == 11);
static_assert(NA_METHOD_DIRECTORY_SET_CURRENT == 12);
static_assert(NA_METHOD_DIRECTORY_SET_ROOT == 13);
static_assert(NA_METHOD_FILE_PREAD == 1);
static_assert(NA_METHOD_FILE_PWRITE == 2);
static_assert(NA_METHOD_FILE_SEEK == 3);
static_assert(NA_METHOD_FILE_STAT == 4);
static_assert(NA_METHOD_FILE_SYNC == 5);
static_assert(NA_METHOD_FILE_TRUNCATE == 6);
static_assert(NA_METHOD_FILE_MATERIALIZE == 17);
static_assert(NA_METHOD_VFS_GET_ROOT == 1);
static_assert(NA_METHOD_VFS_PREPARE_MOUNT == 2);
static_assert(NA_METHOD_VFS_UNMOUNT == 3);
static_assert(NA_METHOD_VFS_SYNC == 4);
static_assert(NA_METHOD_VFS_GET_MOUNT_INFO == 5);
static_assert(NA_METHOD_BLOCK_DEVICE_GET_INFO == 1);
static_assert(NA_METHOD_BLOCK_DEVICE_READ == 2);
static_assert(NA_METHOD_BLOCK_DEVICE_WRITE == 3);
static_assert(NA_METHOD_BLOCK_DEVICE_FLUSH == 4);
static_assert(NA_METHOD_BLOCK_DEVICE_DISCARD == 5);
static_assert(NA_METHOD_BLOCK_DEVICE_FACTORY_GET_INFO == 1);
static_assert(NA_METHOD_BLOCK_DEVICE_FACTORY_ACQUIRE == 2);
static_assert(NA_METHOD_MOUNT_TICKET_COMMIT == 1);
static_assert(NA_METHOD_MOUNT_TICKET_ABORT == 2);
static_assert(NA_METHOD_MOUNT_TICKET_STATUS == 3);
static_assert(NA_METHOD_FILE_ALLOCATE == 7);
static_assert(NA_METHOD_FILE_GET_FLAGS == 8);
static_assert(NA_METHOD_FILE_SET_FLAGS == 9);
static_assert(NA_METHOD_FILE_DEVICE_CONTROL == 10);
static_assert(NA_METHOD_FILE_READ == 11);
static_assert(NA_METHOD_FILE_WRITE == 12);
static_assert(NA_METHOD_SERVICE_DIRECTORY_LISTEN == 5);
static_assert(NA_METHOD_SERVICE_DIRECTORY_CONNECT == 6);
static_assert(NA_METHOD_PROCESS_WAIT == 1);
static_assert(NA_METHOD_PROCESS_GET_INFO == 2);
static_assert(NA_METHOD_PROCESS_WAIT_CHILDREN == 3);
static_assert(NA_METHOD_PROCESS_GET_JOB_CONTROL_INFO == 4);
static_assert(NA_METHOD_PROCESS_SET_SESSION == 5);
static_assert(NA_METHOD_PROCESS_GET_PROCESS_GROUP == 6);
static_assert(NA_METHOD_PROCESS_SET_PROCESS_GROUP == 7);
static_assert(NA_METHOD_PROCESS_GET_SESSION == 8);
static_assert(NA_METHOD_PROCESS_GET_CONTROLLING_TERMINAL == 9);
static_assert(NA_METHOD_PROCESS_GET_STATUS == 11);
static_assert(NA_METHOD_PROCESS_GET_COMMAND_LINE == 12);
static_assert(NA_METHOD_PROCESS_LIST_THREADS == 13);
static_assert(NA_SCOPE_SYSTEM_STATUS == 24);
static_assert(NA_METHOD_SYSTEM_STATUS_GET == 1);
static_assert(NA_METHOD_SYSTEM_STATUS_LIST_PROCESSES == 2);
static_assert(NA_SYSTEM_STATUS_FLAG_HAS_SWAP == 1);
static_assert(NA_METHOD_MEMORY_OBJECT_GET_INFO == 1);
static_assert(NA_METHOD_MEMORY_OBJECT_READ == 2);
static_assert(NA_METHOD_MEMORY_OBJECT_WRITE == 3);
static_assert(NA_METHOD_FRAMEBUFFER_GET == 1);
static_assert(NA_METHOD_ACPI_RESOURCE_AUTHORITY_BIND_PCI_CONFIG_ROOT == 4);
static_assert(NA_METHOD_ACPI_RESOURCE_AUTHORITY_BIND_PLATFORM_GENERATION == 8);
static_assert(NA_METHOD_ACPI_RESOURCE_AUTHORITY_VALIDATE_PCI_ROOT_FEATURES == 6);
static_assert(NA_METHOD_ACPI_RESOURCE_AUTHORITY_MINT_PCI_ROOT_RESOURCE_ATTESTATION == 7);
static_assert(NA_METHOD_PCI_DISCOVERY_FABRIC_BIND_ROOT == 2);
static_assert(NA_SCOPE_PCI_DISCOVERY_FABRIC == 31);
static_assert(NA_SCOPE_PCI_DMA_DOMAIN == 37);
static_assert(NA_SCOPE_INTERRUPT_SOURCE == 38);
static_assert(NA_SCOPE_PCI_ROOT_RESOURCE_ATTESTATION == 39);
static_assert(NA_METHOD_PCI_ROOT_RESOURCE_ATTESTATION_GET_AUTHORITY == 1);
static_assert(NA_METHOD_PCI_ROOT_RESOURCE_ATTESTATION_MINT_DEVICE_RESOURCES == 2);
static_assert(NA_SCOPE_PCI_DEVICE_RESOURCE_ATTESTATION == 40);
static_assert(NA_METHOD_PCI_DEVICE_RESOURCE_ATTESTATION_GET_AUTHORITY == 1);
static_assert(NA_SCOPE_PCI_DMA_FAULT_SOURCE == 41);
static_assert(NA_SCOPE_PLATFORM_POWER_CONTROL == 42);
static_assert(NA_SCOPE_PCI_BLOCK_DEVICE_CATALOG == 43);
static_assert(NA_METHOD_PCI_FUNCTION_BIND_DEVICE_RESOURCES == 4);
static_assert(NA_METHOD_PCI_FUNCTION_OPEN_BAR == 5);
static_assert(NA_METHOD_PCI_FUNCTION_READ_CONFIG == 6);
static_assert(NA_METHOD_PCI_FUNCTION_GRANT_DRIVER == 7);
static_assert(NA_PCI_FUNCTION_RIGHT_GRANT_DRIVER == (1ULL << 34));
static_assert(NA_METHOD_PLATFORM_PCI_TOPOLOGY_TAKE_ROOT_RESOURCE_ATTESTATION == 6);
static_assert(NA_METHOD_PLATFORM_PCI_TOPOLOGY_TAKE_DEVICE_RESOURCE_ATTESTATION == 7);
static_assert(NA_METHOD_PLATFORM_PCI_TOPOLOGY_GET_INTERRUPT_ROUTING == 8);
static_assert(NA_METHOD_ACPI_FIXED_HARDWARE_REPORT_SCI_FAILURE == 10);
static_assert(NA_METHOD_ACPI_FIXED_HARDWARE_CLEAR_SCI_STATUS == 11);
static_assert(NA_METHOD_ACPI_FIXED_HARDWARE_ABORT_TRANSITION == 12);
static_assert(NA_METHOD_ACPI_FIXED_HARDWARE_CLAIM_TRANSITION_OWNER == 13);
static_assert(NA_METHOD_PLATFORM_POWER_CONTROL_POWER_OFF == 1);
static_assert(NA_METHOD_PLATFORM_POWER_CONTROL_REBOOT == 2);
static_assert(NA_SCOPE_PLATFORM_POWER_CONTROL == 42);
static_assert(naos::system::AcpiResourceAuthority::revision == 1);

naoidl::bounded_string test_firmware_version()
{
    static constexpr char version[] = "01000000000000000200000000000000";
    return {version, sizeof(version) - 1};
}

TEST_CASE("AcpiFixedHardware IDL reports SCI failure with a sticky degraded state", "[system-idl]")
{
    CHECK(naos::system::AcpiFixedHardware::descriptor.revision == 1);
    CHECK(naos::system::AcpiFixedHardware::descriptor.method_count == 13);

    naos::system::AcpiFixedHardware::describe_response description{};
    description.firmware_version = test_firmware_version();
    description.value.gpe1_base = 0x40;
    std::uint8_t buffer[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::AcpiFixedHardware::encode_describe_response(buffer, sizeof(buffer), description, written));
    naos::system::AcpiFixedHardware::describe_response decoded_description{};
    REQUIRE(naos::system::AcpiFixedHardware::decode_describe_response(buffer, written, decoded_description));
    CHECK(decoded_description.value.gpe1_base == 0x40);
    CHECK(decoded_description.firmware_version.size == test_firmware_version().size);

    naos::system::AcpiFixedHardware::clear_sci_status_request clear_request{};
    clear_request.firmware_version = test_firmware_version();
    clear_request.sequence = 13;
    clear_request.pm1_event_mask = 0x01;
    clear_request.pm1_event_status_b_mask = 0x02;
    clear_request.gpe0_mask = 0x04;
    clear_request.gpe1_mask = 0x08;
    REQUIRE(naos::system::AcpiFixedHardware::encode_clear_sci_status_request(buffer, sizeof(buffer), clear_request,
                                                                             written));
    naos::system::AcpiFixedHardware::clear_sci_status_request decoded_clear_request{};
    REQUIRE(naos::system::AcpiFixedHardware::decode_clear_sci_status_request(buffer, written, decoded_clear_request));
    CHECK(decoded_clear_request.sequence == 13);
    CHECK(decoded_clear_request.pm1_event_mask == 0x01);
    CHECK(decoded_clear_request.pm1_event_status_b_mask == 0x02);
    CHECK(decoded_clear_request.gpe0_mask == 0x04);
    CHECK(decoded_clear_request.gpe1_mask == 0x08);

    naos::system::AcpiFixedHardware::report_sci_failure_request request{};
    request.firmware_version = test_firmware_version();

    REQUIRE(
        naos::system::AcpiFixedHardware::encode_report_sci_failure_request(buffer, sizeof(buffer), request, written));
    naos::system::AcpiFixedHardware::report_sci_failure_request decoded_request{};
    REQUIRE(naos::system::AcpiFixedHardware::decode_report_sci_failure_request(buffer, written, decoded_request));
    CHECK(decoded_request.firmware_version.size == test_firmware_version().size);

    naos::system::AcpiFixedHardware::report_sci_failure_response response{};
    response.sci_state = 2;
    REQUIRE(
        naos::system::AcpiFixedHardware::encode_report_sci_failure_response(buffer, sizeof(buffer), response, written));
    naos::system::AcpiFixedHardware::report_sci_failure_response decoded_response{};
    REQUIRE(naos::system::AcpiFixedHardware::decode_report_sci_failure_response(buffer, written, decoded_response));
    CHECK(decoded_response.sci_state == 2);

    naos::system::AcpiFixedHardware::abort_transition_request abort_request{};
    abort_request.ticket = 21;
    abort_request.firmware_version = test_firmware_version();
    REQUIRE(naos::system::AcpiFixedHardware::encode_abort_transition_request(buffer, sizeof(buffer), abort_request,
                                                                             written));
    naos::system::AcpiFixedHardware::abort_transition_request decoded_abort_request{};
    REQUIRE(naos::system::AcpiFixedHardware::decode_abort_transition_request(buffer, written, decoded_abort_request));
    CHECK(decoded_abort_request.ticket == 21);
    CHECK(decoded_abort_request.firmware_version.size == test_firmware_version().size);

    naos::system::AcpiFixedHardware::claim_transition_owner_request owner_request{};
    owner_request.firmware_version = test_firmware_version();
    REQUIRE(naos::system::AcpiFixedHardware::encode_claim_transition_owner_request(buffer, sizeof(buffer),
                                                                                   owner_request, written));
    naos::system::AcpiFixedHardware::claim_transition_owner_request decoded_owner_request{};
    REQUIRE(
        naos::system::AcpiFixedHardware::decode_claim_transition_owner_request(buffer, written, decoded_owner_request));
    CHECK(decoded_owner_request.firmware_version.size == test_firmware_version().size);
}

TEST_CASE("NaoIDL string fields have no schema length bound", "[system-idl]")
{
    char firmware_version[1024];
    for (std::size_t index = 0; index < sizeof(firmware_version); ++index)
        firmware_version[index] = static_cast<char>('a' + (index % 26));

    naos::system::FirmwareRoot::describe_response response{};
    response.firmware_version = {firmware_version, sizeof(firmware_version)};
    std::uint8_t wire[2048]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::FirmwareRoot::encode_describe_response(wire, sizeof(wire), response, written));
    naos::system::FirmwareRoot::describe_response decoded{};
    REQUIRE(naos::system::FirmwareRoot::decode_describe_response(wire, written, decoded));
    CHECK(decoded.firmware_version.size == sizeof(firmware_version));
    CHECK(decoded.firmware_version.data[0] == firmware_version[0]);
    CHECK(decoded.firmware_version.data[sizeof(firmware_version) - 1] ==
          firmware_version[sizeof(firmware_version) - 1]);
}

TEST_CASE("kernel parses the opaque firmware version string", "[system-idl]")
{
    const auto encoded = firmware::encode_version(0x8877665544332211ULL, 0x1020304050607080ULL);
    naoidl::bounded_string token{encoded.data(), static_cast<std::uint32_t>(encoded.size())};
    std::uint64_t boot_epoch = 0;
    std::uint64_t firmware_generation = 0;
    REQUIRE(firmware::decode_version(token, boot_epoch, firmware_generation));
    CHECK(boot_epoch == 0x8877665544332211ULL);
    CHECK(firmware_generation == 0x1020304050607080ULL);

    token.size -= 1;
    CHECK_FALSE(firmware::decode_version(token, boot_epoch, firmware_generation));
}

TEST_CASE("PlatformPowerControl IDL preserves typed transition outcomes", "[system-idl]")
{
    CHECK(naos::system::PlatformPowerControl::descriptor.revision == 1);
    CHECK(naos::system::PlatformPowerControl::descriptor.method_count == 2);

    naos::system::PlatformPowerControl::power_off_request request{};
    std::uint8_t buffer[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::PlatformPowerControl::encode_power_off_request(buffer, sizeof(buffer), request, written));
    naos::system::PlatformPowerControl::power_off_request decoded_request{};
    REQUIRE(naos::system::PlatformPowerControl::decode_power_off_request(buffer, written, decoded_request));

    naos::system::PlatformPowerControl::power_off_response response{};
    response.result.outcome = 2;
    REQUIRE(naos::system::PlatformPowerControl::encode_power_off_response(buffer, sizeof(buffer), response, written));
    naos::system::PlatformPowerControl::power_off_response decoded_response{};
    REQUIRE(naos::system::PlatformPowerControl::decode_power_off_response(buffer, written, decoded_response));
    CHECK(decoded_response.result.outcome == 2);

    naos::system::PlatformPowerControl::reboot_response reboot_response{};
    reboot_response.result.outcome = 0;
    REQUIRE(
        naos::system::PlatformPowerControl::encode_reboot_response(buffer, sizeof(buffer), reboot_response, written));
    naos::system::PlatformPowerControl::reboot_response decoded_reboot_response{};
    REQUIRE(naos::system::PlatformPowerControl::decode_reboot_response(buffer, written, decoded_reboot_response));
    CHECK(decoded_reboot_response.result.outcome == 0);
}

TEST_CASE("ACPI resource authority IDL carries PCI_Config scope and path", "[system-idl]")
{
    naos::system::AcpiResourceAuthority::authorize_region_request request{};
    request.firmware_version = test_firmware_version();
    request.request.namespace_generation = 13;
    request.request.parent_device_id = 14;
    request.request.base = 0x60;
    request.request.length = 0x0c;
    request.request.space = 2;
    request.request.mode = 0;
    request.request.pci_config_scope_id = 15;
    request.request.pci_root_id = 16;
    request.request.pci_segment = 3;
    request.request.pci_root_start_bus = 32;
    request.request.pci_root_end_bus = 63;
    request.request.pci_path_length = 2;
    request.request.pci_device_function_path = {1, 0x42};

    std::uint8_t buffer[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;
    REQUIRE(
        naos::system::AcpiResourceAuthority::encode_authorize_region_request(buffer, sizeof(buffer), request, written));
    naos::system::AcpiResourceAuthority::authorize_region_request decoded{};
    REQUIRE(naos::system::AcpiResourceAuthority::decode_authorize_region_request(buffer, written, decoded));
    CHECK(decoded.firmware_version.size == test_firmware_version().size);
    CHECK(decoded.request.namespace_generation == 13);
    CHECK(decoded.request.base == 0x60);
    CHECK(decoded.request.pci_config_scope_id == 15);
    CHECK(decoded.request.pci_root_id == 16);
    CHECK(decoded.request.pci_segment == 3);
    CHECK(decoded.request.pci_root_start_bus == 32);
    CHECK(decoded.request.pci_root_end_bus == 63);
    CHECK(decoded.request.pci_path_length == 2);
    CHECK(decoded.request.pci_device_function_path[0] == 1);
    CHECK(decoded.request.pci_device_function_path[1] == 0x42);

    naos::system::AcpiResourceAuthority::bind_pci_config_root_request bind_root_request{};
    bind_root_request.firmware_version = test_firmware_version();
    bind_root_request.request.namespace_generation = 13;
    bind_root_request.request.root_id = 14;
    bind_root_request.request.segment = 2;
    bind_root_request.request.start_bus = 32;
    bind_root_request.request.end_bus = 63;
    bind_root_request.request.platform_topology_generation = 15;
    bind_root_request.request.compatibility_profile_hash = 16;
    REQUIRE(naos::system::AcpiResourceAuthority::encode_bind_pci_config_root_request(buffer, sizeof(buffer),
                                                                                     bind_root_request, written));
    naos::system::AcpiResourceAuthority::bind_pci_config_root_request decoded_bind_root_request{};
    REQUIRE(naos::system::AcpiResourceAuthority::decode_bind_pci_config_root_request(buffer, written,
                                                                                     decoded_bind_root_request));
    CHECK(decoded_bind_root_request.request.platform_topology_generation == 15);
    CHECK(decoded_bind_root_request.request.compatibility_profile_hash == 16);

    naos::system::AcpiResourceAuthority::bind_pci_config_root_response response{};
    response.response.scope_id = 0x1122;
    REQUIRE(naos::system::AcpiResourceAuthority::encode_bind_pci_config_root_response(buffer, sizeof(buffer), response,
                                                                                      written));
    naos::system::AcpiResourceAuthority::bind_pci_config_root_response decoded_response{};
    REQUIRE(
        naos::system::AcpiResourceAuthority::decode_bind_pci_config_root_response(buffer, written, decoded_response));
    CHECK(decoded_response.response.scope_id == 0x1122);

    naos::system::AcpiResourceAuthority::bind_execution_thread_request bind_request{};
    bind_request.firmware_version = test_firmware_version();
    bind_request.request.execution_thread_id = 0x3344;
    REQUIRE(naos::system::AcpiResourceAuthority::encode_bind_execution_thread_request(buffer, sizeof(buffer),
                                                                                      bind_request, written));
    naos::system::AcpiResourceAuthority::bind_execution_thread_request decoded_bind_request{};
    REQUIRE(naos::system::AcpiResourceAuthority::decode_bind_execution_thread_request(buffer, written,
                                                                                      decoded_bind_request));
    CHECK(decoded_bind_request.request.execution_thread_id == 0x3344);
}

TEST_CASE("ACPI executor validates _OSC evidence without minting a capability", "[system-idl]")
{
    naos::system::AcpiResourceAuthority::validate_pci_root_features_request request{};
    request.firmware_version = test_firmware_version();
    request.evidence.pci_config_scope_id = 17;
    request.evidence.root_id = 18;
    request.evidence.aml_namespace_generation = 21;
    request.evidence.platform_topology_generation = 22;
    request.evidence.pci_fabric_generation = 23;
    request.evidence.compatibility_profile_hash = 24;
    request.evidence.segment = 2;
    request.evidence.start_bus = 32;
    request.evidence.end_bus = 63;
    request.evidence.requested_support = 1u << 3;
    request.evidence.requested_control = 1u << 4;
    request.evidence.query_support = 1u << 3;
    request.evidence.query_granted_control = 1u << 4;
    request.evidence.commit_support = 1u << 3;
    request.evidence.granted_control = 1u << 4;
    request.evidence.denied_control = 0;
    request.evidence.masked_control = 0;

    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::AcpiResourceAuthority::encode_validate_pci_root_features_request(wire, sizeof(wire), request,
                                                                                           written));
    naos::system::AcpiResourceAuthority::validate_pci_root_features_request decoded{};
    REQUIRE(naos::system::AcpiResourceAuthority::decode_validate_pci_root_features_request(wire, written, decoded));
    CHECK(decoded.evidence.pci_config_scope_id == 17);
    CHECK(decoded.evidence.granted_control == (1u << 4));
    CHECK(naos::system::AcpiResourceAuthority::method_validate_pci_root_features == 6);
    CHECK(naos::system::AcpiResourceAuthority::revision == 1);
}

TEST_CASE("ACPI executor mints a typed root resource attestation from bounded CRS windows", "[system-idl]")
{
    naos::system::AcpiResourceAuthority::mint_pci_root_resource_attestation_request request{};
    request.firmware_version = test_firmware_version();
    request.pci_config_scope_id = 17;
    request.root_id = 18;
    request.aml_namespace_generation = 21;
    request.platform_topology_generation = 22;
    request.pci_fabric_generation = 23;
    request.compatibility_profile_hash = 24;
    request.segment = 2;
    request.start_bus = 32;
    request.end_bus = 63;
    request.windows.data = request.windows_storage.data();
    request.windows.count = 2;
    request.windows.data[0] = {2, 1, 0, 0, 0, 32, 63, 0, 32, 0};
    request.windows.data[1] = {0, 1, 0, 0, 0xffff, 0x80000000, 0x80ffffff, 0, 0x01000000, 0};

    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::AcpiResourceAuthority::encode_mint_pci_root_resource_attestation_request(wire, sizeof(wire),
                                                                                                   request, written));
    naos::system::AcpiResourceAuthority::mint_pci_root_resource_attestation_request decoded{};
    REQUIRE(
        naos::system::AcpiResourceAuthority::decode_mint_pci_root_resource_attestation_request(wire, written, decoded));
    CHECK(decoded.pci_config_scope_id == 17);
    REQUIRE(decoded.windows.count == 2);
    CHECK(decoded.windows.data[1].minimum == 0x80000000);
    CHECK(decoded.windows.data[1].length == 0x01000000);

    naos::system::AcpiResourceAuthority::mint_pci_root_resource_attestation_response response{};
    response.authority.value = 0;
    REQUIRE(naos::system::AcpiResourceAuthority::validate_mint_pci_root_resource_attestation_response_resources(
        response, 1));
    CHECK_FALSE(naos::system::AcpiResourceAuthority::validate_mint_pci_root_resource_attestation_response_resources(
        response, 0));

    naos::system::PciRootResourceAttestation::get_authority_response authority{};
    authority.root_id = 18;
    authority.firmware_version = test_firmware_version();
    authority.memory_windows.data = authority.memory_windows_storage.data();
    authority.memory_windows.count = 1;
    authority.memory_windows.data[0] = {0x80000000, 0x01000000};
    std::uint8_t authority_wire[256]{};
    REQUIRE(naos::system::PciRootResourceAttestation::encode_get_authority_response(
        authority_wire, sizeof(authority_wire), authority, written));
    naos::system::PciRootResourceAttestation::get_authority_response decoded_authority{};
    REQUIRE(naos::system::PciRootResourceAttestation::decode_get_authority_response(authority_wire, written,
                                                                                    decoded_authority));
    CHECK(decoded_authority.root_id == 18);
    REQUIRE(decoded_authority.memory_windows.count == 1);
    CHECK(decoded_authority.memory_windows.data[0].base == 0x80000000);

    naos::system::PciRootResourceAttestation::mint_device_resources_request device_request{};
    device_request.device_id = 20;
    device_request.path_length = 2;
    device_request.device_function_path = {0x08, 0x13};
    device_request.memory_windows.data = device_request.memory_windows_storage.data();
    device_request.memory_windows.count = 1;
    device_request.memory_windows.data[0] = {0x80f00000, 0x1000, 1};
    std::uint8_t device_request_wire[256]{};
    REQUIRE(naos::system::PciRootResourceAttestation::encode_mint_device_resources_request(
        device_request_wire, sizeof(device_request_wire), device_request, written));
    naos::system::PciRootResourceAttestation::mint_device_resources_request decoded_device_request{};
    REQUIRE(naos::system::PciRootResourceAttestation::decode_mint_device_resources_request(device_request_wire, written,
                                                                                           decoded_device_request));
    CHECK(decoded_device_request.device_id == 20);
    CHECK(decoded_device_request.path_length == 2);
    CHECK(decoded_device_request.device_function_path[1] == 0x13);
    REQUIRE(decoded_device_request.memory_windows.count == 1);
    CHECK(decoded_device_request.memory_windows.data[0].base == 0x80f00000);
    CHECK(decoded_device_request.memory_windows.data[0].flags == 1);

    CHECK(naos::system::PciRootResourceAttestation::revision == 1);
    CHECK(naos::system::PciDeviceResourceAttestation::revision == 1);
}

TEST_CASE("PCI root binding requires exactly one typed root resource attestation", "[system-idl]")
{
    naos::system::PciDiscoveryFabric::bind_root_request request{};
    request.index = 1;
    request.platform_root_id = 2;
    request.firmware_version = test_firmware_version();
    request.aml_namespace_generation = 5;
    request.platform_topology_generation = 6;
    request.pci_fabric_generation = 7;
    request.segment = 0;
    request.start_bus = 0;
    request.end_bus = 255;
    request.attestation.value = 0;

    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::PciDiscoveryFabric::encode_bind_root_request(wire, sizeof(wire), request, written));
    naos::system::PciDiscoveryFabric::bind_root_request decoded{};
    REQUIRE(naos::system::PciDiscoveryFabric::decode_bind_root_request(wire, written, decoded));
    CHECK(decoded.attestation.value == 0);
    CHECK(naos::system::PciDiscoveryFabric::validate_bind_root_request_resources(decoded, 1));
    CHECK_FALSE(naos::system::PciDiscoveryFabric::validate_bind_root_request_resources(decoded, 0));
}

TEST_CASE("PCI function creates a scoped DMA domain and DMA maps duplicate MemoryObject pages", "[system-idl]")
{
    naos::system::PciFunction::create_dma_domain_response create_response{};
    create_response.domain.value = 0;
    create_response.faults.value = 1;
    CHECK(naos::system::PciFunction::descriptor.revision == 1);
    CHECK(naos::system::PciFunction::descriptor.method_count == 7);
    CHECK(naos::system::PciDmaDomain::descriptor.scope == NA_SCOPE_PCI_DMA_DOMAIN);
    CHECK(naos::system::PciDmaFaultSource::descriptor.scope == NA_SCOPE_PCI_DMA_FAULT_SOURCE);
    CHECK(naos::system::PciFunction::validate_create_dma_domain_response_resources(create_response, 2));
    CHECK_FALSE(naos::system::PciFunction::validate_create_dma_domain_response_resources(create_response, 1));
    CHECK_FALSE(naos::system::PciFunction::validate_create_dma_domain_response_resources(create_response, 0));

    naos::system::PciDmaDomain::map_page_request request{};
    request.iova = 0x4000;
    request.object_offset = 0x2000;
    request.direction = 0; // device reads from the MemoryObject
    request.backing.value = 0;

    std::uint8_t wire[128]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::PciDmaDomain::encode_map_page_request(wire, sizeof(wire), request, written));
    naos::system::PciDmaDomain::map_page_request decoded{};
    REQUIRE(naos::system::PciDmaDomain::decode_map_page_request(wire, written, decoded));
    CHECK(decoded.iova == 0x4000);
    CHECK(decoded.object_offset == 0x2000);
    CHECK(decoded.backing.value == 0);
    CHECK(naos::system::PciDmaDomain::validate_map_page_request_resources(request, 1));
    CHECK_FALSE(naos::system::PciDmaDomain::validate_map_page_request_resources(request, 0));

    naos::system::PciFunction::bind_device_resources_request bind_request{};
    bind_request.device_id = 0x1234;
    bind_request.authority.value = 0;
    CHECK(naos::system::PciFunction::validate_bind_device_resources_request_resources(bind_request, 1));
    CHECK_FALSE(naos::system::PciFunction::validate_bind_device_resources_request_resources(bind_request, 0));

    naos::system::PciFunction::open_bar_request open_request{};
    open_request.index = 0;
    naos::system::PciFunction::open_bar_response open_response{};
    open_response.length = 0x1000;
    open_response.mapping.value = 0;
    CHECK(naos::system::PciFunction::validate_open_bar_response_resources(open_response, 1));
    CHECK_FALSE(naos::system::PciFunction::validate_open_bar_response_resources(open_response, 0));
    na_resource_disposition_t bar_mapping{};
    bar_mapping.operation = NA_RESOURCE_MOVE;
    bar_mapping.scope = NA_SCOPE_MEMORY_OBJECT;
    bar_mapping.rights = NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_INFO;
    CHECK(naos::system::PciFunction::validate_open_bar_response_dispositions(open_response, &bar_mapping, 1));

    naos::system::PciFunction::read_config_request config_request{};
    config_request.offset = 2;
    config_request.width = 2;
    std::uint8_t config_wire[32]{};
    std::uint64_t config_written = 0;
    REQUIRE(naos::system::PciFunction::encode_read_config_request(config_wire, sizeof(config_wire), config_request,
                                                                  config_written));
    naos::system::PciFunction::read_config_request decoded_config_request{};
    REQUIRE(naos::system::PciFunction::decode_read_config_request(config_wire, config_written, decoded_config_request));
    CHECK(decoded_config_request.offset == 2);
    CHECK(decoded_config_request.width == 2);

    naos::system::PciFunction::read_config_response config_response{};
    config_response.value = 0x11e8;
    REQUIRE(naos::system::PciFunction::encode_read_config_response(config_wire, sizeof(config_wire), config_response,
                                                                   config_written));
    naos::system::PciFunction::read_config_response decoded_config_response{};
    REQUIRE(
        naos::system::PciFunction::decode_read_config_response(config_wire, config_written, decoded_config_response));
    CHECK(decoded_config_response.value == 0x11e8);
    CHECK((naos::system::PciFunction::descriptor.method_rights[5] & NA_PCI_FUNCTION_RIGHT_READ_CONFIG) != 0);
    CHECK((naos::system::PciFunction::descriptor.method_rights[1] & NA_PCI_FUNCTION_RIGHT_CREATE_DMA_DOMAIN) != 0);
    CHECK((naos::system::PciFunction::descriptor.method_rights[3] & NA_PCI_FUNCTION_RIGHT_BIND_DEVICE_RESOURCES) != 0);
    CHECK((naos::system::PciFunction::descriptor.method_rights[4] & NA_PCI_FUNCTION_RIGHT_OPEN_BAR) != 0);

    naos::system::PciFunction::grant_driver_request grant_request{};
    grant_request.profile.rights = 0x5;
    grant_request.profile.bar_mask = 0x3;
    grant_request.driver_process.value = 0;
    std::uint8_t grant_wire[64]{};
    std::uint64_t grant_written = 0;
    REQUIRE(naos::system::PciFunction::encode_grant_driver_request(grant_wire, sizeof(grant_wire), grant_request,
                                                                   grant_written));
    naos::system::PciFunction::grant_driver_request decoded_grant_request{};
    REQUIRE(naos::system::PciFunction::decode_grant_driver_request(grant_wire, grant_written, decoded_grant_request));
    CHECK(decoded_grant_request.profile.rights == 0x5);
    CHECK(decoded_grant_request.profile.bar_mask == 0x3);
    CHECK(decoded_grant_request.driver_process.value == 0);
    CHECK(naos::system::PciFunction::validate_grant_driver_request_resources(grant_request, 1));
    CHECK_FALSE(naos::system::PciFunction::validate_grant_driver_request_resources(grant_request, 0));
    na_resource_disposition_t process_disposition{};
    process_disposition.operation = NA_RESOURCE_MOVE;
    process_disposition.scope = NA_SCOPE_PROCESS;
    process_disposition.rights = NA_RIGHT_TRANSFER;
    CHECK(
        naos::system::PciFunction::validate_grant_driver_request_dispositions(grant_request, &process_disposition, 1));

    naos::system::PciFunction::grant_driver_response grant_response{};
    grant_response.service_directory.value = 0;
    CHECK(naos::system::PciFunction::validate_grant_driver_response_resources(grant_response, 1));
    CHECK_FALSE(naos::system::PciFunction::validate_grant_driver_response_resources(grant_response, 0));
    na_resource_disposition_t directory_disposition{};
    directory_disposition.operation = NA_RESOURCE_MOVE;
    directory_disposition.scope = NA_SCOPE_SERVICE_DIRECTORY;
    directory_disposition.rights = NA_RIGHT_TRANSFER;
    CHECK(naos::system::PciFunction::validate_grant_driver_response_dispositions(grant_response, &directory_disposition,
                                                                                 1));
    CHECK((naos::system::PciFunction::descriptor.method_rights[6] & NA_PCI_FUNCTION_RIGHT_GRANT_DRIVER) != 0);
}

TEST_CASE("PCI DMA fault source uses a bounded typed take request", "[system-idl]")
{
    CHECK(naos::system::PciDmaFaultSource::descriptor.revision == 1);
    CHECK(naos::system::PciDmaFaultSource::descriptor.method_count == 1);
    naos::system::PciDmaFaultSource::take_request request{};
    request.limit = 16;

    std::uint8_t wire[32]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::PciDmaFaultSource::encode_take_request(wire, sizeof(wire), request, written));
    naos::system::PciDmaFaultSource::take_request decoded{};
    REQUIRE(naos::system::PciDmaFaultSource::decode_take_request(wire, written, decoded));
    CHECK(decoded.limit == 16);
    CHECK(naos::system::PciDmaFaultSource::validate_take_request_resources(request, 0));
    CHECK_FALSE(naos::system::PciDmaFaultSource::validate_take_request_resources(request, 1));

    naos::system::PciDmaFaultSource::take_response response{};
    response.sequence = 9;
    response.unit_overflow = 1;
    response.firmware_version = test_firmware_version();
    response.aml_namespace_generation = 103;
    response.platform_topology_generation = 104;
    response.pci_fabric_generation = 105;
    response.records.data = response.records_storage.data();
    std::uint8_t response_wire[128]{};
    REQUIRE(
        naos::system::PciDmaFaultSource::encode_take_response(response_wire, sizeof(response_wire), response, written));
    naos::system::PciDmaFaultSource::take_response decoded_response{};
    REQUIRE(naos::system::PciDmaFaultSource::decode_take_response(response_wire, written, decoded_response));
    CHECK(decoded_response.sequence == 9);
    CHECK(decoded_response.unit_overflow == 1);
    CHECK(decoded_response.firmware_version.size == test_firmware_version().size);
    CHECK(decoded_response.aml_namespace_generation == 103);
    CHECK(decoded_response.platform_topology_generation == 104);
    CHECK(decoded_response.pci_fabric_generation == 105);
    CHECK(decoded_response.count == 0);
}

TEST_CASE("ACPI SCI event and acknowledgement round trip the kernel generation lineage", "[system-idl]")
{
    CHECK(naos::system::AcpiFixedHardware::descriptor.revision == 1);
    naos::system::AcpiFixedHardware::drain_sci_response event_response{};
    event_response.firmware_version = test_firmware_version();
    event_response.event.sequence = 77;
    event_response.event.flags = 1;
    event_response.event.pm1_event_status = 2;
    event_response.event.aml_namespace_generation = 103;
    event_response.event.platform_topology_generation = 104;
    event_response.event.pci_fabric_generation = 105;

    std::uint8_t wire[256]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::AcpiFixedHardware::encode_drain_sci_response(wire, sizeof(wire), event_response, written));
    naos::system::AcpiFixedHardware::drain_sci_response decoded_event{};
    REQUIRE(naos::system::AcpiFixedHardware::decode_drain_sci_response(wire, written, decoded_event));
    CHECK(decoded_event.firmware_version.size == test_firmware_version().size);
    CHECK(decoded_event.event.aml_namespace_generation == 103);
    CHECK(decoded_event.event.platform_topology_generation == 104);
    CHECK(decoded_event.event.pci_fabric_generation == 105);

    naos::system::AcpiFixedHardware::ack_and_rearm_sci_response ack_response{};
    ack_response.firmware_version = test_firmware_version();
    ack_response.ack.sequence = 77;
    ack_response.ack.state = 1;
    ack_response.ack.flags = 2;
    ack_response.ack.aml_namespace_generation = 103;
    ack_response.ack.platform_topology_generation = 104;
    ack_response.ack.pci_fabric_generation = 105;
    REQUIRE(
        naos::system::AcpiFixedHardware::encode_ack_and_rearm_sci_response(wire, sizeof(wire), ack_response, written));
    naos::system::AcpiFixedHardware::ack_and_rearm_sci_response decoded_ack{};
    REQUIRE(naos::system::AcpiFixedHardware::decode_ack_and_rearm_sci_response(wire, written, decoded_ack));
    CHECK(decoded_ack.firmware_version.size == test_firmware_version().size);
    CHECK(decoded_ack.ack.aml_namespace_generation == 103);
    CHECK(decoded_ack.ack.platform_topology_generation == 104);
    CHECK(decoded_ack.ack.pci_fabric_generation == 105);
}

TEST_CASE("PCI interrupt source is owner-bound and has separate method rights", "[system-idl]")
{
    CHECK(naos::system::InterruptSource::descriptor.scope == NA_SCOPE_INTERRUPT_SOURCE);
    CHECK(naos::system::InterruptSource::descriptor.revision == 1);
    CHECK((naos::system::PciFunction::descriptor.method_rights[2] & NA_PCI_FUNCTION_RIGHT_ROUTE_INTERRUPT) != 0);
    CHECK((naos::system::InterruptSource::descriptor.method_rights[0] & NA_INTERRUPT_SOURCE_RIGHT_BIND) != 0);
    CHECK((naos::system::InterruptSource::descriptor.method_rights[1] & NA_INTERRUPT_SOURCE_RIGHT_WAIT) != 0);
    CHECK((naos::system::InterruptSource::descriptor.method_rights[2] & NA_INTERRUPT_SOURCE_RIGHT_MASK) != 0);
    CHECK((naos::system::InterruptSource::descriptor.method_rights[3] & NA_INTERRUPT_SOURCE_RIGHT_ACK) != 0);

    naos::system::PciFunction::route_interrupt_response response{};
    response.source.value = 0;
    CHECK(naos::system::PciFunction::validate_route_interrupt_response_resources(response, 1));
    CHECK_FALSE(naos::system::PciFunction::validate_route_interrupt_response_resources(response, 0));

    na_resource_disposition_t disposition{};
    disposition.operation = NA_RESOURCE_MOVE;
    disposition.scope = NA_SCOPE_INTERRUPT_SOURCE;
    CHECK(naos::system::PciFunction::validate_route_interrupt_response_dispositions(response, &disposition, 1));
}

TEST_CASE("PCI topology IDL preserves root device identity and resource windows", "[system-idl]")
{
    naos::system::PlatformPciTopology::get_pci_roots_response response{};
    response.firmware_version = test_firmware_version();
    response.roots.count = 1;
    response.roots.data = response.roots_storage.data();
    auto &root = response.roots.data[0];
    root.root_id = 0x10;
    root.segment = 0;
    root.start_bus = 32;
    root.end_bus = 63;
    root.aml_namespace_generation = 3;
    root.platform_topology_generation = 4;
    root.pci_fabric_generation = 5;
    root.device_id = 0x20;
    root.resource_window_start = 0;
    root.resource_window_count = 2;
    root.compatibility_profile_hash = 0xfeedcafe12345678;

    response.resource_windows.count = 2;
    response.resource_windows.data = response.resource_windows_storage.data();
    response.resource_windows.data[0] = {
        .resource_type = 0,
        .descriptor_revision = 1,
        .general_flags = 12,
        .type_specific_flags = 3,
        .granularity = 0,
        .minimum = 0x80000000,
        .maximum = 0x8fffffff,
        .translation_offset = 0,
        .length = 0x10000000,
        .type_specific_attributes = 0,
    };
    response.resource_windows.data[1] = {
        .resource_type = 2,
        .descriptor_revision = 0,
        .general_flags = 12,
        .type_specific_flags = 0,
        .granularity = 0,
        .minimum = 32,
        .maximum = 63,
        .translation_offset = 0,
        .length = 32,
        .type_specific_attributes = 0,
    };

    std::uint8_t wire[1024]{};
    std::uint64_t written = 0;
    REQUIRE(naos::system::PlatformPciTopology::encode_get_pci_roots_response(wire, sizeof(wire), response, written));
    naos::system::PlatformPciTopology::get_pci_roots_response decoded{};
    REQUIRE(naos::system::PlatformPciTopology::decode_get_pci_roots_response(wire, written, decoded));
    REQUIRE(decoded.roots.count == 1);
    CHECK(decoded.roots.data[0].root_id == 0x10);
    CHECK(decoded.roots.data[0].device_id == 0x20);
    CHECK(decoded.roots.data[0].resource_window_count == 2);
    CHECK(decoded.roots.data[0].compatibility_profile_hash == 0xfeedcafe12345678);
    REQUIRE(decoded.resource_windows.count == 2);
    CHECK(decoded.resource_windows.data[0].minimum == 0x80000000);
    CHECK(decoded.resource_windows.data[1].resource_type == 2);
    CHECK(naos::system::PlatformPciTopology::descriptor.revision == 1);

    naos::system::PlatformPciTopology::negotiate_pci_features_response feature_response{};
    feature_response.firmware_version = test_firmware_version();
    feature_response.value.root_id = 0x10;
    feature_response.value.aml_namespace_generation = 3;
    feature_response.value.platform_topology_generation = 4;
    feature_response.value.pci_fabric_generation = 5;
    std::uint8_t feature_wire[128]{};
    REQUIRE(naos::system::PlatformPciTopology::encode_negotiate_pci_features_response(
        feature_wire, sizeof(feature_wire), feature_response, written));
    naos::system::PlatformPciTopology::negotiate_pci_features_response decoded_feature{};
    REQUIRE(naos::system::PlatformPciTopology::decode_negotiate_pci_features_response(feature_wire, written,
                                                                                      decoded_feature));
    CHECK(decoded_feature.value.pci_fabric_generation == 5);

    naos::system::PlatformPciTopology::generation_response generation{};
    generation.firmware_version = test_firmware_version();
    generation.value.aml_namespace_generation = 3;
    generation.value.platform_topology_generation = 4;
    generation.value.pci_fabric_generation = 5;
    generation.value.health = 1;
    generation.value.compatibility_profile_hash = 0xfeedcafe12345678;
    std::uint8_t generation_wire[128]{};
    REQUIRE(naos::system::PlatformPciTopology::encode_generation_response(generation_wire, sizeof(generation_wire),
                                                                          generation, written));
    naos::system::PlatformPciTopology::generation_response decoded_generation{};
    REQUIRE(
        naos::system::PlatformPciTopology::decode_generation_response(generation_wire, written, decoded_generation));
    CHECK(decoded_generation.value.compatibility_profile_hash == 0xfeedcafe12345678);
}

TEST_CASE("PCI topology device snapshots and resource queries use bounded flat arrays", "[system-idl]")
{
    using namespace naos::system::PlatformPciTopology;

    list_devices_response response{};
    response.devices.count = 1;
    response.devices.data = response.devices_storage.data();
    response.devices.data[0] = {
        .device_id = 0x11,
        .namespace_generation = 0x22,
        .status = 0xf,
        .evaluation_flags = 0x1f,
        .initialized = 1,
        .degraded = 0,
        .identifier_start = 0,
        .identifier_count = 1,
        .resource_start = 0,
        .resource_count = 1,
        .routing_entries = 2,
        .pci_root_id = 7,
        .pci_path_start = 0,
        .pci_path_count = 2,
    };
    response.identifiers.count = 1;
    response.identifiers.data = response.identifiers_storage.data();
    response.identifiers.data[0] = {
        .kind = 1,
        .value_kind = 1,
        .text_start = 0,
        .text_length = 7,
        .integer = 0,
    };
    response.resources.count = 1;
    response.resources.data = response.resources_storage.data();
    response.resources.data[0] = {
        .kind = 2,
        .base = 0x80000000,
        .length = 0x1000,
        .flags = 3,
    };
    response.identifier_text.count = 7;
    response.identifier_text.data = response.identifier_text_storage.data();
    std::memcpy(response.identifier_text.data, "PNP0A08", 7);
    response.next_cursor = 0;
    response.pci_device_function_path.count = 2;
    response.pci_device_function_path.data = response.pci_device_function_path_storage.data();
    response.pci_device_function_path.data[0] = 1;
    response.pci_device_function_path.data[1] = 0x23;

    std::array<std::uint8_t, 65536> wire{};
    std::uint64_t written = 0;
    REQUIRE(encode_list_devices_response(wire.data(), wire.size(), response, written));
    list_devices_response decoded{};
    REQUIRE(decode_list_devices_response(wire.data(), written, decoded));
    REQUIRE(decoded.devices.count == 1);
    CHECK(decoded.devices.data[0].device_id == 0x11);
    CHECK(decoded.devices.data[0].evaluation_flags == 0x1f);
    CHECK(decoded.devices.data[0].pci_root_id == 7);
    CHECK(decoded.devices.data[0].pci_path_count == 2);
    REQUIRE(decoded.pci_device_function_path.count == 2);
    CHECK(decoded.pci_device_function_path.data[0] == 1);
    CHECK(decoded.pci_device_function_path.data[1] == 0x23);
    REQUIRE(decoded.identifiers.count == 1);
    CHECK(decoded.identifiers.data[0].text_length == 7);
    REQUIRE(decoded.identifier_text.count == 7);
    CHECK(std::memcmp(decoded.identifier_text.data, "PNP0A08", 7) == 0);
    REQUIRE(decoded.resources.count == 1);
    CHECK(decoded.resources.data[0].base == 0x80000000);

    get_resources_response resources{};
    resources.resources.count = 1;
    resources.resources.data = resources.resources_storage.data();
    resources.resources.data[0] = response.resources.data[0];
    resources.evaluated = 1;
    resources.degraded = 0;
    std::array<std::uint8_t, 8192> resource_wire{};
    REQUIRE(encode_get_resources_response(resource_wire.data(), resource_wire.size(), resources, written));
    get_resources_response decoded_resources{};
    REQUIRE(decode_get_resources_response(resource_wire.data(), written, decoded_resources));
    REQUIRE(decoded_resources.resources.count == 1);
    CHECK(decoded_resources.resources.data[0].length == 0x1000);
    CHECK(decoded_resources.evaluated == 1);
}

TEST_CASE("Platform PCI routing IDL preserves bounded typed route pages", "[system-idl]")
{
    using namespace naos::system::PlatformPciTopology;

    get_interrupt_routing_response response{};
    response.routes.count = 2;
    response.routes.data = response.routes_storage.data();
    response.routes.data[0] = {
        .address = 0x0002ffff,
        .pin = 0,
        .source_kind = PciRoutingSourceKind::direct_gsi,
        .source_device_id = 0,
        .source_index = 23,
    };
    response.routes.data[1] = {
        .address = 0x0003ffff,
        .pin = 3,
        .source_kind = PciRoutingSourceKind::link_device,
        .source_device_id = 0xabc,
        .source_index = 1,
    };
    response.next_cursor = 2;
    response.evaluated = 1;
    response.complete = 1;

    std::array<std::uint8_t, 4096> wire{};
    std::uint64_t written = 0;
    REQUIRE(encode_get_interrupt_routing_response(wire.data(), wire.size(), response, written));
    get_interrupt_routing_response decoded{};
    REQUIRE(decode_get_interrupt_routing_response(wire.data(), written, decoded));
    REQUIRE(decoded.routes.count == 2);
    CHECK(decoded.routes.data[0].address == 0x0002ffff);
    CHECK(decoded.routes.data[0].source_kind == PciRoutingSourceKind::direct_gsi);
    CHECK(decoded.routes.data[0].source_index == 23);
    CHECK(decoded.routes.data[1].source_kind == PciRoutingSourceKind::link_device);
    CHECK(decoded.routes.data[1].source_device_id == 0xabc);
    CHECK(decoded.routes.data[1].source_index == 1);
    CHECK(decoded.next_cursor == 2);
    CHECK(decoded.evaluated == 1);
    CHECK(decoded.complete == 1);
}

TEST_CASE("generated system IDL contract", "[system-idl]")
{
    naos::system::SystemStatus::SystemStatus system_status{};
    system_status.page_size = 4096;
    system_status.physical_pages = 32768;
    system_status.usable_pages = 30000;
    system_status.free_pages = 12000;
    system_status.reclaimable_pages = 4000;
    system_status.swap_total_pages = 0;
    system_status.swap_free_pages = 0;
    system_status.swap_used_pages = 0;
    REQUIRE(system_status.flags == 0);
    REQUIRE(system_status.free_pages + system_status.reclaimable_pages == 16000);

    naos::system::SystemStatus::list_processes_response process_page{};
    process_page.next_after_pid = 0;
    process_page.processes.count = 1;
    process_page.processes.data = process_page.processes_storage.data();
    process_page.processes.data[0].pid = 42;
    process_page.processes.data[0].process.value = 0;
    std::uint8_t process_page_wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t process_page_written = 0;
    REQUIRE(naos::system::SystemStatus::encode_list_processes_response(process_page_wire, sizeof(process_page_wire),
                                                                       process_page, process_page_written));
    naos::system::SystemStatus::list_processes_response decoded_process_page{};
    REQUIRE(naos::system::SystemStatus::decode_list_processes_response(process_page_wire, process_page_written,
                                                                       decoded_process_page));
    REQUIRE(decoded_process_page.processes.count == 1);
    REQUIRE(decoded_process_page.processes.data[0].pid == 42);
    REQUIRE(decoded_process_page.processes.data[0].process.value == 0);

    naos::system::Process::ProcessStatus process_status{};
    process_status.page_size = system_status.page_size;
    process_status.virtual_pages = 256;
    process_status.rss_pages = 64;
    process_status.shared_pages = 8;
    REQUIRE(process_status.rss_pages >= process_status.shared_pages);

    std::uint8_t buffer[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;

    naos::system::Process::get_command_line_request command_line_request{};
    command_line_request.size = 4096;
    command_line_request.buffer.value = 0;
    REQUIRE(
        naos::system::Process::encode_get_command_line_request(buffer, sizeof(buffer), command_line_request, written));
    naos::system::Process::get_command_line_request decoded_command_line_request{};
    REQUIRE(naos::system::Process::decode_get_command_line_request(buffer, written, decoded_command_line_request));
    REQUIRE(decoded_command_line_request.size == 4096);
    REQUIRE(decoded_command_line_request.buffer.value == 0);

    naos::system::Process::get_command_line_response command_line_response{};
    command_line_response.actual_bytes = 23;
    command_line_response.required_bytes = 23;
    REQUIRE(naos::system::Process::encode_get_command_line_response(buffer, sizeof(buffer), command_line_response,
                                                                    written));
    naos::system::Process::get_command_line_response decoded_command_line_response{};
    REQUIRE(naos::system::Process::decode_get_command_line_response(buffer, written, decoded_command_line_response));
    REQUIRE(decoded_command_line_response.actual_bytes == 23);
    REQUIRE(decoded_command_line_response.required_bytes == 23);

    naos::system::Process::list_threads_response thread_page{};
    thread_page.next_after_tid = 0;
    thread_page.threads.count = 1;
    thread_page.threads.data = thread_page.threads_storage.data();
    thread_page.threads.data[0].tid = 7;
    thread_page.threads.data[0].state = 1;
    thread_page.threads.data[0].attributes = 16;
    thread_page.threads.data[0].cpu_id = 0;
    thread_page.threads.data[0].static_priority = 125;
    thread_page.threads.data[0].dynamic_priority = -2;
    thread_page.threads.data[0].user_time_us = 10;
    thread_page.threads.data[0].system_time_us = 20;
    REQUIRE(naos::system::Process::encode_list_threads_response(buffer, sizeof(buffer), thread_page, written));
    naos::system::Process::list_threads_response decoded_thread_page{};
    REQUIRE(naos::system::Process::decode_list_threads_response(buffer, written, decoded_thread_page));
    REQUIRE(decoded_thread_page.threads.count == 1);
    REQUIRE(decoded_thread_page.threads.data[0].tid == 7);
    REQUIRE(decoded_thread_page.threads.data[0].dynamic_priority == -2);

    // Terminal endpoints must carry method-level authority, rather than
    // relying on ttyd's handler to rediscover O_RDONLY/O_WRONLY.  The
    // generated descriptor is the contract consumed by invoke_submit().
    static_assert(naos::system::TerminalMaster::descriptor.method_rights[0] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_TERMINAL_RIGHT_READ));
    static_assert(naos::system::TerminalMaster::descriptor.method_rights[1] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_TERMINAL_RIGHT_WRITE));
    static_assert(naos::system::TerminalManager::descriptor
                      .method_rights[naos::system::TerminalManager::descriptor.method_count] == 0);
    static_assert(!has_legacy_job_control_process_id<naos::system::TerminalJobControl::check_io_request>);
    // BlockDevice methods are individually authorized by the frozen
    // per-protocol named rights; get_info carries only block_inspect.
    static_assert(naos::system::BlockDevice::descriptor.method_rights[0] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_INSPECT));
    static_assert(naos::system::BlockDevice::descriptor.method_rights[1] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_READ));
    static_assert(naos::system::BlockDevice::descriptor.method_rights[2] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_WRITE));
    static_assert(naos::system::BlockDevice::descriptor.method_rights[3] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_FLUSH));
    static_assert(naos::system::BlockDevice::descriptor.method_rights[4] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_DISCARD));
    static_assert(naos::system::BlockDeviceFactory::descriptor.method_rights[1] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_FACTORY_RIGHT_ACQUIRE));
    static_assert(naos::system::Framebuffer::descriptor.method_rights[0] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_DISPLAY_RIGHT_WRITER));
    static_assert(naos::system::SystemStatus::descriptor.method_rights[0] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_SYSTEM_STATUS_RIGHT_INSPECT));

    // Generated enum decoders are closed: an unknown wire value must not be
    // interpreted as the first action by a handler switch.
    const std::uint8_t unknown_driver_action[] = {0xff, 0, 0, 0};
    naos::system::TerminalDriverControl::raise_foreground_request invalid_driver_request{};
    REQUIRE(!naos::system::TerminalDriverControl::decode_raise_foreground_request(
        unknown_driver_action, sizeof(unknown_driver_action), invalid_driver_request));

    const std::uint8_t payload[] = {1, 2, 3};
    // Stream payloads travel through the caller's MemoryObject region; the
    // request wire only names the region window and the requested size.
    naos::system::Stream::write_request write_request{};
    write_request.size = sizeof(payload);
    write_request.flags = 0;
    write_request.buffer.value = 0;
    REQUIRE(naos::system::Stream::encode_write_request(buffer, sizeof(buffer), write_request, written));
    REQUIRE(written == 20);
    naos::system::Stream::write_request decoded_write{};
    REQUIRE(naos::system::Stream::decode_write_request(buffer, written, decoded_write));
    REQUIRE(decoded_write.size == sizeof(payload));
    REQUIRE(decoded_write.flags == 0);
    REQUIRE(decoded_write.buffer.value == 0);

    naos::system::Stream::read_response stream_read{};
    stream_read.count = sizeof(payload);
    REQUIRE(naos::system::Stream::encode_read_response(buffer, sizeof(buffer), stream_read, written));
    REQUIRE(written == 8);
    naos::system::Stream::read_response decoded_stream_read{};
    REQUIRE(naos::system::Stream::decode_read_response(buffer, written, decoded_stream_read));
    REQUIRE(decoded_stream_read.count == sizeof(payload));

    naos::system::File::seek_request seek_request{};
    seek_request.offset = -7;
    seek_request.whence = 2;
    REQUIRE(naos::system::File::encode_seek_request(buffer, sizeof(buffer), seek_request, written));
    REQUIRE(written == 16);
    naos::system::File::seek_request decoded_seek{};
    REQUIRE(naos::system::File::decode_seek_request(buffer, written, decoded_seek));
    REQUIRE(decoded_seek.offset == -7);
    REQUIRE(decoded_seek.whence == 2);

    naos::system::File::read_request file_read_request{};
    file_read_request.size = sizeof(payload);
    file_read_request.flags = 0;
    file_read_request.buffer.value = 0;
    REQUIRE(naos::system::File::encode_read_request(buffer, sizeof(buffer), file_read_request, written));
    REQUIRE(written == 20);
    naos::system::File::read_request decoded_file_read{};
    REQUIRE(naos::system::File::decode_read_request(buffer, written, decoded_file_read));
    REQUIRE(decoded_file_read.size == sizeof(payload));
    REQUIRE(decoded_file_read.buffer.value == 0);

    // pread carries the resource slot alongside its offset/flags; the byte
    // count matches the generated header layout.
    naos::system::File::pread_request pread_request{};
    pread_request.offset = 128;
    pread_request.size = sizeof(payload);
    pread_request.flags = 0;
    pread_request.buffer.value = 0;
    REQUIRE(naos::system::File::encode_pread_request(buffer, sizeof(buffer), pread_request, written));
    REQUIRE(written == 28);
    naos::system::File::pread_request decoded_pread{};
    REQUIRE(naos::system::File::decode_pread_request(buffer, written, decoded_pread));
    REQUIRE(decoded_pread.offset == 128);
    REQUIRE(decoded_pread.size == sizeof(payload));
    REQUIRE(decoded_pread.buffer.value == 0);
    naos::system::File::pread_response pread_response{};
    pread_response.count = sizeof(payload);
    REQUIRE(naos::system::File::encode_pread_response(buffer, sizeof(buffer), pread_response, written));
    REQUIRE(written == 8);
    naos::system::File::pread_response decoded_pread_response{};
    REQUIRE(naos::system::File::decode_pread_response(buffer, written, decoded_pread_response));
    REQUIRE(decoded_pread_response.count == sizeof(payload));

    // getcwd() uses the empty Directory::PATH request. Keep its wire shape
    // covered as well, since the native ABI requires nullptr for zero bytes.
    naos::system::Directory::path_request path_request{};
    REQUIRE(naos::system::Directory::encode_path_request(buffer, sizeof(buffer), path_request, written));
    REQUIRE(written == 0);
    REQUIRE(naos::system::Directory::decode_path_request(nullptr, 0, path_request));

    // Framebuffer::get has the same zero-field request shape.  The kernel
    // dispatcher invokes this generated decoder, so malformed non-empty
    // payloads must be rejected instead of being treated as a valid get.
    naos::system::Framebuffer::get_request framebuffer_get{};
    REQUIRE(naos::system::Framebuffer::encode_get_request(nullptr, 0, framebuffer_get, written));
    REQUIRE(written == 0);
    REQUIRE(naos::system::Framebuffer::decode_get_request(nullptr, 0, framebuffer_get));
    const std::uint8_t extra_framebuffer_byte = 0;
    REQUIRE_FALSE(naos::system::Framebuffer::decode_get_request(&extra_framebuffer_byte, sizeof(extra_framebuffer_byte),
                                                                framebuffer_get));
    REQUIRE_FALSE(naos::system::Framebuffer::decode_get_request(nullptr, 1, framebuffer_get));

    naos::system::Directory::open_request open_request{};
    open_request.mode = 1;
    const std::uint8_t path[] = {'/', 0};
    open_request.path = {path, sizeof(path)};
    REQUIRE(naos::system::Directory::encode_open_request(buffer, sizeof(buffer), open_request, written));
    REQUIRE(written == 18);
    naos::system::Directory::open_request decoded_open{};
    REQUIRE(naos::system::Directory::decode_open_request(buffer, written, decoded_open));
    REQUIRE(decoded_open.path.size == sizeof(path));
    REQUIRE(decoded_open.path.data[0] == '/');

    naos::system::Process::wait_response wait_response{};
    wait_response.status = -1;
    wait_response.pid = 42;
    wait_response.page_faults = 7;
    wait_response.peak_rss_pages = 11;
    wait_response.page_size = 4096;
    wait_response.user_stack_pages = 13;
    wait_response.file_inputs = 17;
    wait_response.file_outputs = 19;
    wait_response.signals_delivered = 23;
    wait_response.voluntary_context_switches = 29;
    wait_response.involuntary_context_switches = 31;
    wait_response.text_pages = 37;
    wait_response.resident_pages = 41;
    REQUIRE(naos::system::Process::encode_wait_response(buffer, sizeof(buffer), wait_response, written));
    REQUIRE(written == 120);
    naos::system::Process::wait_response decoded_wait{};
    REQUIRE(naos::system::Process::decode_wait_response(buffer, written, decoded_wait));
    REQUIRE(decoded_wait.status == -1);
    REQUIRE(decoded_wait.pid == 42);
    REQUIRE(decoded_wait.page_faults == 7);
    REQUIRE(decoded_wait.peak_rss_pages == 11);
    REQUIRE(decoded_wait.page_size == 4096);
    REQUIRE(decoded_wait.user_stack_pages == 13);
    REQUIRE(decoded_wait.file_inputs == 17);
    REQUIRE(decoded_wait.file_outputs == 19);
    REQUIRE(decoded_wait.signals_delivered == 23);
    REQUIRE(decoded_wait.voluntary_context_switches == 29);
    REQUIRE(decoded_wait.involuntary_context_switches == 31);
    REQUIRE(decoded_wait.text_pages == 37);
    REQUIRE(decoded_wait.resident_pages == 41);

    naos::system::Process::wait_children_request wait_children_request{};
    wait_children_request.pid = -1;
    wait_children_request.flags = 1;
    REQUIRE(
        naos::system::Process::encode_wait_children_request(buffer, sizeof(buffer), wait_children_request, written));
    REQUIRE(written == 16);
    naos::system::Process::wait_children_request decoded_wait_children{};
    REQUIRE(naos::system::Process::decode_wait_children_request(buffer, written, decoded_wait_children));
    REQUIRE(decoded_wait_children.pid == -1);
    REQUIRE(decoded_wait_children.flags == 1);

    naos::system::Process::get_job_control_info_response job_control_info{};
    job_control_info.session = 11;
    job_control_info.process_group = 12;
    job_control_info.foreground_process_group = 13;
    job_control_info.has_controlling_tty = 1;
    REQUIRE(
        naos::system::Process::encode_get_job_control_info_response(buffer, sizeof(buffer), job_control_info, written));
    REQUIRE(written == 32);
    naos::system::Process::get_job_control_info_response decoded_job_control_info{};
    REQUIRE(naos::system::Process::decode_get_job_control_info_response(buffer, written, decoded_job_control_info));
    REQUIRE(decoded_job_control_info.session == 11);
    REQUIRE(decoded_job_control_info.process_group == 12);
    REQUIRE(decoded_job_control_info.foreground_process_group == 13);
    REQUIRE(decoded_job_control_info.has_controlling_tty == 1);

    naos::system::Process::set_process_group_request set_process_group_request{};
    set_process_group_request.process_group = 17;
    REQUIRE(naos::system::Process::encode_set_process_group_request(buffer, sizeof(buffer), set_process_group_request,
                                                                    written));
    REQUIRE(written == 8);
    naos::system::Process::set_process_group_request decoded_set_process_group{};
    REQUIRE(naos::system::Process::decode_set_process_group_request(buffer, written, decoded_set_process_group));
    REQUIRE(decoded_set_process_group.process_group == 17);

    naos::system::Process::set_session_response set_session{};
    set_session.session = 19;
    REQUIRE(naos::system::Process::encode_set_session_response(buffer, sizeof(buffer), set_session, written));
    REQUIRE(written == 8);
    naos::system::Process::set_session_response decoded_set_session{};
    REQUIRE(naos::system::Process::decode_set_session_response(buffer, written, decoded_set_session));
    REQUIRE(decoded_set_session.session == 19);

    naos::system::Process::get_process_group_response process_group{};
    process_group.process_group = 23;
    REQUIRE(naos::system::Process::encode_get_process_group_response(buffer, sizeof(buffer), process_group, written));
    REQUIRE(written == 8);
    naos::system::Process::get_process_group_response decoded_process_group{};
    REQUIRE(naos::system::Process::decode_get_process_group_response(buffer, written, decoded_process_group));
    REQUIRE(decoded_process_group.process_group == 23);

    naos::system::Process::get_session_response session{};
    session.session = 29;
    REQUIRE(naos::system::Process::encode_get_session_response(buffer, sizeof(buffer), session, written));
    REQUIRE(written == 8);
    naos::system::Process::get_session_response decoded_session{};
    REQUIRE(naos::system::Process::decode_get_session_response(buffer, written, decoded_session));
    REQUIRE(decoded_session.session == 29);

    // MemoryObject payloads moved into a sibling region on the same object;
    // both directions name the window and reply the transferred byte count.
    naos::system::MemoryObject::write_request memory_write{};
    memory_write.offset = 4096;
    memory_write.size = sizeof(payload);
    memory_write.buffer.value = 0;
    REQUIRE(naos::system::MemoryObject::encode_write_request(buffer, sizeof(buffer), memory_write, written));
    REQUIRE(written == 20);

    naos::system::MemoryObject::write_request decoded_memory_write{};
    REQUIRE(naos::system::MemoryObject::decode_write_request(buffer, written, decoded_memory_write));
    REQUIRE(decoded_memory_write.offset == 4096);
    REQUIRE(decoded_memory_write.size == sizeof(payload));
    REQUIRE(decoded_memory_write.buffer.value == 0);

    naos::system::MemoryObject::read_request memory_read{};
    memory_read.offset = 8192;
    memory_read.size = sizeof(payload);
    memory_read.buffer.value = 0;
    REQUIRE(naos::system::MemoryObject::encode_read_request(buffer, sizeof(buffer), memory_read, written));
    REQUIRE(written == 20);
    naos::system::MemoryObject::read_request decoded_memory_read{};
    REQUIRE(naos::system::MemoryObject::decode_read_request(buffer, written, decoded_memory_read));
    REQUIRE(decoded_memory_read.offset == 8192);
    REQUIRE(decoded_memory_read.size == sizeof(payload));
    naos::system::MemoryObject::read_response memory_read_response{};
    memory_read_response.count = sizeof(payload);
    REQUIRE(naos::system::MemoryObject::encode_read_response(buffer, sizeof(buffer), memory_read_response, written));
    REQUIRE(written == 8);
    naos::system::MemoryObject::read_response decoded_memory_read_response{};
    REQUIRE(naos::system::MemoryObject::decode_read_response(buffer, written, decoded_memory_read_response));
    REQUIRE(decoded_memory_read_response.count == sizeof(payload));

    // stat_node replaces the open+stat simulation.
    naos::system::Directory::stat_node_request stat_node_request{};
    stat_node_request.flags = 1; // NA_DIRECTORY_LOOKUP_FLAG_NOFOLLOW
    const std::uint8_t node_path[] = {'a', 'b'};
    stat_node_request.path_size = sizeof(node_path);
    stat_node_request.path = {node_path, sizeof(node_path)};
    REQUIRE(naos::system::Directory::encode_stat_node_request(buffer, sizeof(buffer), stat_node_request, written));
    REQUIRE(written == 18);
    naos::system::Directory::stat_node_request decoded_stat_node{};
    REQUIRE(naos::system::Directory::decode_stat_node_request(buffer, written, decoded_stat_node));
    REQUIRE(decoded_stat_node.flags == 1);
    REQUIRE(decoded_stat_node.path.size == sizeof(node_path));

    // Directory::list records now land in the caller's region; the request
    // names the window and the response reports records/bytes written, not
    // the record bytes themselves.
    naos::system::Directory::list_request directory_list{};
    directory_list.offset = 0;
    directory_list.requested_bytes = 4096;
    directory_list.buffer.value = 0;
    REQUIRE(naos::system::Directory::encode_list_request(buffer, sizeof(buffer), directory_list, written));
    REQUIRE(written == 20);
    naos::system::Directory::list_request decoded_directory_list{};
    REQUIRE(naos::system::Directory::decode_list_request(buffer, written, decoded_directory_list));
    REQUIRE(decoded_directory_list.offset == 0);
    REQUIRE(decoded_directory_list.requested_bytes == 4096);
    REQUIRE(decoded_directory_list.buffer.value == 0);
    naos::system::Directory::list_response directory_list_response{};
    directory_list_response.next = 2;
    directory_list_response.count = 3;
    directory_list_response.bytes = 96;
    REQUIRE(naos::system::Directory::encode_list_response(buffer, sizeof(buffer), directory_list_response, written));
    REQUIRE(written == 24);
    naos::system::Directory::list_response decoded_directory_list_response{};
    REQUIRE(naos::system::Directory::decode_list_response(buffer, written, decoded_directory_list_response));
    REQUIRE(decoded_directory_list_response.next == 2);
    REQUIRE(decoded_directory_list_response.count == 3);
    REQUIRE(decoded_directory_list_response.bytes == 96);

    // File materialize returns a move-only MemoryObject handle plus the
    // snapshot length/generation (handle travels in the resource table).
    naos::system::File::materialize_response materialize_response{};
    materialize_response.length = 4096;
    materialize_response.generation = 7;
    REQUIRE(naos::system::File::encode_materialize_response(buffer, sizeof(buffer), materialize_response, written));
    REQUIRE(written == 20);
    REQUIRE(naos::system::File::decode_materialize_response(buffer, written, materialize_response));
    REQUIRE(materialize_response.length == 4096);
    REQUIRE(materialize_response.generation == 7);
}
