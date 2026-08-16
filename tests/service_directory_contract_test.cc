#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <naos/generated/system/ServiceDirectory.hpp>
#include <naos/generated/system_uapi.h>
#include <naos/service_directory.hpp>

int run_service_directory_contract_tests()
{
    using namespace naos::system::ServiceDirectory;

    assert(protocol_scope == NA_SCOPE_SERVICE_DIRECTORY);
    assert(protocol_uuid.bytes[15] == 0x0a);
    assert(method_register == 1);
    assert(method_resolve == 2);
    assert(method_unregister == 3);
    assert(method_list == 4);
    assert(method_listen == 5);
    assert(method_connect == 6);

    using register_handle_function = int (*)(const char *, na_handle_t);
    static_assert(std::is_same_v<decltype(&naos_service_register_handle), register_handle_function>);

    register_request register_value{};
    const char uri[] = "naos://system/console";
    static_assert(std::is_same_v<decltype(register_value.uri), naoidl::bounded_string>);
    register_value.uri = {uri, sizeof(uri) - 1};
    register_value.service.value = 0;

    std::uint8_t buffer[512]{};
    std::uint64_t written = 0;
    assert(encode_register_request(buffer, sizeof(buffer), register_value, written));

    register_request decoded{};
    assert(decode_register_request(buffer, written, decoded));
    assert(decoded.uri.size == sizeof(uri) - 1);
    assert(decoded.uri.data[0] == 'n');
    assert(decoded.service.value == 0);

    constexpr std::size_t max_uri_bytes = 65536;
    constexpr std::size_t prefix_bytes = sizeof("naos://") - 1;
    std::string max_uri = "naos://" + std::string(max_uri_bytes - prefix_bytes, 'a');
    resolve_request max_request{};
    max_request.uri = {max_uri.data(), static_cast<std::uint32_t>(max_uri.size())};
    std::vector<std::uint8_t> max_buffer(max_uri.size());
    assert(encode_resolve_request(max_buffer.data(), max_buffer.size(), max_request, written));
    assert(written == max_uri.size());
    resolve_request max_decoded{};
    assert(decode_resolve_request(max_buffer.data(), written, max_decoded));
    assert(max_decoded.uri.size == max_uri.size());

    na_resource_disposition_t disposition{};
    disposition.operation = NA_RESOURCE_MOVE;
    disposition.rights = NA_RIGHT_TRANSFER;
    assert(validate_register_request_resources(decoded, 1));
    assert(validate_register_request_dispositions(decoded, &disposition, 1));

    disposition.operation = NA_RESOURCE_DUPLICATE;
    assert(!validate_register_request_dispositions(decoded, &disposition, 1));

    resolve_response response{};
    response.service.value = 0;
    assert(validate_resolve_response_resources(response, 1));
    assert(encode_resolve_response(buffer, sizeof(buffer), response, written));
    resolve_response decoded_response{};
    assert(decode_resolve_response(buffer, written, decoded_response));
    assert(decoded_response.service.value == 0);

    listen_request listen_value{};
    listen_value.max_pending = 16;
    listen_value.listener.value = 0;
    listen_value.descriptor.value = 1;
    listen_value.uri = {uri, sizeof(uri) - 1};
    assert(encode_listen_request(buffer, sizeof(buffer), listen_value, written));
    assert(written == 16 + sizeof(uri) - 1);
    listen_request decoded_listen{};
    assert(decode_listen_request(buffer, written, decoded_listen));
    assert(decoded_listen.max_pending == 16);
    assert(decoded_listen.listener.value == 0);
    assert(decoded_listen.descriptor.value == 1);
    assert(decoded_listen.uri.size == sizeof(uri) - 1);

    connect_request connect_value{};
    const std::uint8_t expected_uuid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::copy(expected_uuid, expected_uuid + 16, connect_value.expected_uuid.begin());
    connect_value.requested_rights = NA_PROTOCOL_RIGHT_INVOKE;
    connect_value.requested_revision = revision;
    connect_value.requested_features = features;
    connect_value.uri = {uri, sizeof(uri) - 1};
    assert(encode_connect_request(buffer, sizeof(buffer), connect_value, written));
    connect_request decoded_connect{};
    assert(decode_connect_request(buffer, written, decoded_connect));
    assert(decoded_connect.expected_uuid[0] == 1 && decoded_connect.expected_uuid[15] == 16);
    assert(decoded_connect.requested_rights == NA_PROTOCOL_RIGHT_INVOKE);
    assert(decoded_connect.requested_revision == revision);
    assert(decoded_connect.requested_features == features);
    assert(decoded_connect.uri.size == sizeof(uri) - 1);

    connect_response connect_response_value{};
    connect_response_value.client.value = 0;
    connect_response_value.revision = revision;
    connect_response_value.features = features;
    assert(validate_connect_response_resources(connect_response_value, 1));
    assert(encode_connect_response(buffer, sizeof(buffer), connect_response_value, written));
    connect_response decoded_connect_response{};
    assert(decode_connect_response(buffer, written, decoded_connect_response));
    assert(decoded_connect_response.client.value == 0);
    assert(decoded_connect_response.revision == revision);
    assert(decoded_connect_response.features == features);

    return 0;
}
