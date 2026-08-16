#include "catch2_compat.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <naos/generated/system/ServiceDirectory.hpp>
#include <naos/generated/system_uapi.h>
#include <naos/service_directory.hpp>

TEST_CASE("service directory contract", "[service-directory]")
{
    using namespace naos::system::ServiceDirectory;

    REQUIRE(protocol_scope == NA_SCOPE_SERVICE_DIRECTORY);
    REQUIRE(protocol_uuid.bytes[15] == 0x0a);
    REQUIRE(method_register == 1);
    REQUIRE(method_resolve == 2);
    REQUIRE(method_unregister == 3);
    REQUIRE(method_list == 4);
    REQUIRE(method_listen == 5);
    REQUIRE(method_connect == 6);

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
}
