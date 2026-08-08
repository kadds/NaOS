#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <naos/generated/system/ServiceDirectory.hpp>
#include <naos/generated/system_uapi.h>
#include <naos/service_directory.hpp>

int main()
{
    using namespace naos::system::ServiceDirectory;

    assert(protocol_scope == NA_SCOPE_SERVICE_DIRECTORY);
    assert(protocol_uuid.bytes[15] == 0x0a);
    assert(method_register == 1);
    assert(method_resolve == 2);
    assert(method_unregister == 3);
    assert(method_list == 4);

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

    return 0;
}
