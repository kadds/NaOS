#include "catch2_compat.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include "kernel/service_directory_policy.hpp"
#include <naos/generated/system/ServiceDirectory.hpp>
#include <naos/generated/system_uapi.h>
#include <naos/service_directory.hpp>

namespace
{
struct uri_scope_fixture
{
    const char *const *uris;
    u64 count;
};

bool uri_scope_contains(const void *opaque, const char *uri, u64 uri_size)
{
    const auto &fixture = *static_cast<const uri_scope_fixture *>(opaque);
    for (u64 index = 0; index < fixture.count; ++index)
    {
        const auto *candidate = fixture.uris[index];
        const auto candidate_size = static_cast<u64>(std::strlen(candidate));
        if (candidate_size == uri_size && std::memcmp(candidate, uri, static_cast<size_t>(uri_size)) == 0)
            return true;
    }
    return false;
}
} // namespace

TEST_CASE("kernel ACPI and PCI locators require a scoped ServiceDirectory view for resolve and connect",
          "[service-directory]")
{
    constexpr const char *protected_uris[] = {
        NAOS_SERVICE_FIRMWARE_ROOT, NAOS_SERVICE_ACPI_FIXED_HARDWARE,   NAOS_SERVICE_ACPI_RESOURCE_AUTHORITY,
        NAOS_SERVICE_PCI_DISCOVERY, NAOS_SERVICE_PLATFORM_PCI_TOPOLOGY,
    };

    for (const auto *uri : protected_uris)
    {
        const auto size = static_cast<std::uint64_t>(std::strlen(uri));
        REQUIRE_FALSE(service::resolve_uri_allowed(uri, size, false));
        REQUIRE(service::resolve_uri_allowed(uri, size, true));
        REQUIRE_FALSE(service::connect_uri_allowed(uri, size, false));
        REQUIRE(service::connect_uri_allowed(uri, size, true));
    }

    constexpr char boot_module[] = NAOS_SERVICE_BOOT_MODULE_INIT;
    REQUIRE(service::resolve_uri_allowed(boot_module, sizeof(boot_module) - 1, false));
    REQUIRE(service::connect_uri_allowed(boot_module, sizeof(boot_module) - 1, false));
    constexpr char filesystem[] = NAOS_SERVICE_VFS;
    REQUIRE(service::resolve_uri_allowed(filesystem, sizeof(filesystem) - 1, false));
    REQUIRE(service::connect_uri_allowed(filesystem, sizeof(filesystem) - 1, false));
    constexpr char power_control[] = NAOS_SERVICE_PLATFORM_POWER_CONTROL;
    REQUIRE(service::resolve_uri_allowed(power_control, sizeof(power_control) - 1, false));
    REQUIRE(service::connect_uri_allowed(power_control, sizeof(power_control) - 1, false));
}

TEST_CASE("nested ServiceDirectory views cannot widen their parent's URI scope", "[service-directory]")
{
    constexpr const char *parent_uris[] = {
        "naos://system/pci/driver/0000-00-03-0",
        "naos://system/pci/driver/0000-00-04-0",
    };
    const uri_scope_fixture parent{parent_uris, 2};
    constexpr const char *narrowed[] = {"naos://system/pci/driver/0000-00-03-0"};
    constexpr const char *expanded[] = {
        "naos://system/pci/driver/0000-00-03-0",
        "naos://system/pci/driver/0000-00-05-0",
    };

    CHECK(service::uri_allowlist_is_subset(narrowed, 1, uri_scope_contains, &parent));
    CHECK_FALSE(service::uri_allowlist_is_subset(expanded, 2, uri_scope_contains, &parent));
    CHECK_FALSE(service::uri_allowlist_is_subset(narrowed, 0, uri_scope_contains, &parent));
    CHECK_FALSE(service::uri_allowlist_is_subset(nullptr, 1, uri_scope_contains, &parent));
    CHECK_FALSE(service::uri_allowlist_is_subset(narrowed, 1, nullptr, &parent));
}

TEST_CASE("early-service bootstrap preserves only explicitly granted directory rights", "[service-directory]")
{
    const auto platformd_rights =
        service::bootstrap_service_directory_rights(NA_SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER);
    CHECK(platformd_rights == (NA_PROTOCOL_RIGHT_INVOKE | NA_SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER));

    const auto pcied_rights = service::bootstrap_service_directory_rights(NA_SERVICE_DIRECTORY_RIGHT_CREATE_VIEW |
                                                                          NA_SERVICE_DIRECTORY_RIGHT_PROVIDER);
    CHECK(pcied_rights ==
          (NA_PROTOCOL_RIGHT_INVOKE | NA_SERVICE_DIRECTORY_RIGHT_CREATE_VIEW | NA_SERVICE_DIRECTORY_RIGHT_PROVIDER));
    CHECK((pcied_rights & NA_SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER) == 0);

    const auto private_registry_rights =
        service::bootstrap_service_directory_rights(NA_SERVICE_DIRECTORY_RIGHT_LOCAL_REGISTER);
    CHECK(private_registry_rights == (NA_PROTOCOL_RIGHT_INVOKE | NA_SERVICE_DIRECTORY_RIGHT_LOCAL_REGISTER));
    CHECK((private_registry_rights & NA_SERVICE_DIRECTORY_RIGHT_SYSTEM_MANAGER) == 0);
    CHECK((private_registry_rights & NA_SERVICE_DIRECTORY_RIGHT_ADMIN) == 0);

    CHECK(service::bootstrap_service_directory_rights(0) == NA_PROTOCOL_RIGHT_INVOKE);

    constexpr u64 platformd_meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    constexpr u64 pcied_meta_rights = platformd_meta_rights | NA_RIGHT_TRANSFER;
    CHECK(service::bootstrap_service_directory_meta_rights(false) == platformd_meta_rights);
    CHECK(service::bootstrap_service_directory_meta_rights(true) == pcied_meta_rights);
}

TEST_CASE("service directory contract", "[service-directory]")
{
    using namespace naos::system::ServiceDirectory;

    REQUIRE(protocol_scope == NA_SCOPE_SERVICE_DIRECTORY);
    REQUIRE(std::any_of(std::begin(protocol_uuid.bytes), std::end(protocol_uuid.bytes),
                        [](std::uint8_t value) { return value != 0; }));
    REQUIRE(method_register == 1);
    REQUIRE(method_resolve == 2);
    REQUIRE(method_unregister == 3);
    REQUIRE(method_list == 4);
    REQUIRE(method_listen == 5);
    REQUIRE(method_connect == 6);
    REQUIRE(method_list_prefix == 7);
    REQUIRE(method_create_view == 8);
    REQUIRE(naos::system::ServiceDirectory::descriptor.revision == 1);
    REQUIRE((naos::system::ServiceDirectory::descriptor.method_rights[7] & NA_SERVICE_DIRECTORY_RIGHT_CREATE_VIEW) !=
            0);

    using register_handle_function = int (*)(const char *, na_handle_t);
    static_assert(std::is_same_v<decltype(&naos_service_register_handle), register_handle_function>);

    register_request register_value{};
    const char uri[] = "naos://system/console";
    static_assert(std::is_same_v<decltype(register_value.uri), naoidl::bounded_string>);
    register_value.uri = {uri, sizeof(uri) - 1};
    register_value.service.value = 0;

    std::uint8_t buffer[512]{};
    std::uint64_t written = 0;
    REQUIRE(encode_register_request(buffer, sizeof(buffer), register_value, written));

    register_request decoded{};
    REQUIRE(decode_register_request(buffer, written, decoded));
    REQUIRE(decoded.uri.size == sizeof(uri) - 1);
    REQUIRE(decoded.uri.data[0] == 'n');
    REQUIRE(decoded.service.value == 0);

    constexpr std::size_t max_uri_bytes = 65536;
    constexpr std::size_t prefix_bytes = sizeof("naos://") - 1;
    std::string max_uri = "naos://" + std::string(max_uri_bytes - prefix_bytes, 'a');
    resolve_request max_request{};
    max_request.uri = {max_uri.data(), static_cast<std::uint32_t>(max_uri.size())};
    std::vector<std::uint8_t> max_buffer(max_uri.size());
    REQUIRE(encode_resolve_request(max_buffer.data(), max_buffer.size(), max_request, written));
    REQUIRE(written == max_uri.size());
    resolve_request max_decoded{};
    REQUIRE(decode_resolve_request(max_buffer.data(), written, max_decoded));
    REQUIRE(max_decoded.uri.size == max_uri.size());

    na_resource_disposition_t disposition{};
    disposition.operation = NA_RESOURCE_MOVE;
    disposition.rights = NA_RIGHT_TRANSFER;
    REQUIRE(validate_register_request_resources(decoded, 1));
    REQUIRE(validate_register_request_dispositions(decoded, &disposition, 1));

    disposition.operation = NA_RESOURCE_DUPLICATE;
    REQUIRE(!validate_register_request_dispositions(decoded, &disposition, 1));

    resolve_response response{};
    response.service.value = 0;
    REQUIRE(validate_resolve_response_resources(response, 1));
    REQUIRE(encode_resolve_response(buffer, sizeof(buffer), response, written));
    resolve_response decoded_response{};
    REQUIRE(decode_resolve_response(buffer, written, decoded_response));
    REQUIRE(decoded_response.service.value == 0);
    disposition.operation = NA_RESOURCE_MOVE;
    disposition.rights = 0;
    REQUIRE(validate_resolve_response_dispositions(decoded_response, &disposition, 1));

    list_prefix_request list_prefix_value{};
    list_prefix_value.offset = 0;
    list_prefix_value.requested_bytes = 1024;
    list_prefix_value.prefix = {uri, sizeof(uri) - 1};
    list_prefix_value.buffer.value = 0;
    REQUIRE(encode_list_prefix_request(buffer, sizeof(buffer), list_prefix_value, written));
    REQUIRE(written == list_prefix_request_header_bytes + sizeof(uri) - 1);
    list_prefix_request decoded_list_prefix{};
    REQUIRE(decode_list_prefix_request(buffer, written, decoded_list_prefix));
    REQUIRE(decoded_list_prefix.offset == 0);
    REQUIRE(decoded_list_prefix.requested_bytes == 1024);
    REQUIRE(decoded_list_prefix.prefix.size == sizeof(uri) - 1);
    REQUIRE(decoded_list_prefix.buffer.value == 0);

    // The records travel through the caller's region; the response only
    // reports how many records and bytes the service wrote there.
    const char record[] = "naos://system/console";
    list_prefix_response list_prefix_response_value{};
    list_prefix_response_value.next = 1;
    list_prefix_response_value.count = 1;
    list_prefix_response_value.bytes = sizeof(record) - 1;
    REQUIRE(encode_list_prefix_response(buffer, sizeof(buffer), list_prefix_response_value, written));
    REQUIRE(written == list_prefix_response_header_bytes);
    list_prefix_response decoded_list_prefix_response{};
    REQUIRE(decode_list_prefix_response(buffer, written, decoded_list_prefix_response));
    REQUIRE(decoded_list_prefix_response.next == 1);
    REQUIRE(decoded_list_prefix_response.count == 1);
    REQUIRE(decoded_list_prefix_response.bytes == sizeof(record) - 1);

    listen_request listen_value{};
    listen_value.max_pending = 16;
    listen_value.listener.value = 0;
    listen_value.descriptor.value = 1;
    listen_value.uri = {uri, sizeof(uri) - 1};
    REQUIRE(encode_listen_request(buffer, sizeof(buffer), listen_value, written));
    REQUIRE(written == 16 + sizeof(uri) - 1);
    listen_request decoded_listen{};
    REQUIRE(decode_listen_request(buffer, written, decoded_listen));
    REQUIRE(decoded_listen.max_pending == 16);
    REQUIRE(decoded_listen.listener.value == 0);
    REQUIRE(decoded_listen.descriptor.value == 1);
    REQUIRE(decoded_listen.uri.size == sizeof(uri) - 1);

    connect_request connect_value{};
    const std::uint8_t expected_uuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::copy(expected_uuid, expected_uuid + 16, connect_value.expected_uuid.begin());
    connect_value.requested_rights = NA_PROTOCOL_RIGHT_INVOKE;
    connect_value.requested_revision = revision;
    connect_value.requested_features = features;
    connect_value.uri = {uri, sizeof(uri) - 1};
    REQUIRE(encode_connect_request(buffer, sizeof(buffer), connect_value, written));
    connect_request decoded_connect{};
    REQUIRE(decode_connect_request(buffer, written, decoded_connect));
    REQUIRE((decoded_connect.expected_uuid[0] == 1 && decoded_connect.expected_uuid[15] == 16));
    REQUIRE(decoded_connect.requested_rights == NA_PROTOCOL_RIGHT_INVOKE);
    REQUIRE(decoded_connect.requested_revision == revision);
    REQUIRE(decoded_connect.requested_features == features);
    REQUIRE(decoded_connect.uri.size == sizeof(uri) - 1);

    connect_response connect_response_value{};
    connect_response_value.client.value = 0;
    connect_response_value.revision = revision;
    connect_response_value.features = features;
    REQUIRE(validate_connect_response_resources(connect_response_value, 1));
    REQUIRE(encode_connect_response(buffer, sizeof(buffer), connect_response_value, written));
    connect_response decoded_connect_response{};
    REQUIRE(decode_connect_response(buffer, written, decoded_connect_response));
    REQUIRE(decoded_connect_response.client.value == 0);
    REQUIRE(decoded_connect_response.revision == revision);
    REQUIRE(decoded_connect_response.features == features);

    create_view_request create_view_value{};
    create_view_value.uri = {uri, sizeof(uri) - 1};
    REQUIRE(encode_create_view_request(buffer, sizeof(buffer), create_view_value, written));
    create_view_request decoded_create_view{};
    REQUIRE(decode_create_view_request(buffer, written, decoded_create_view));
    REQUIRE(decoded_create_view.uri.size == sizeof(uri) - 1);

    create_view_response create_view_response_value{};
    create_view_response_value.view.value = 0;
    REQUIRE(validate_create_view_response_resources(create_view_response_value, 1));
    REQUIRE_FALSE(validate_create_view_response_resources(create_view_response_value, 0));
    REQUIRE(encode_create_view_response(buffer, sizeof(buffer), create_view_response_value, written));
    create_view_response decoded_create_view_response{};
    REQUIRE(decode_create_view_response(buffer, written, decoded_create_view_response));
    REQUIRE(decoded_create_view_response.view.value == 0);
    disposition.operation = NA_RESOURCE_MOVE;
    disposition.scope = NA_SCOPE_SERVICE_DIRECTORY;
    disposition.rights = NA_RIGHT_TRANSFER;
    REQUIRE(validate_create_view_response_dispositions(decoded_create_view_response, &disposition, 1));
}
