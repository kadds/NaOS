#include <cassert>
#include <cstdint>
#include <type_traits>

#include <naos/service_directory.hpp>
#include <naos/generated/system/ServiceDirectory.hpp>
#include <naos/generated/system_uapi.h>

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
    const std::uint8_t uri[] = {'n', 'a', 'o', 's', ':', '/', '/', 's', 'y', 's', 't', 'e', 'm', '/', 'c', 'o',
                                'n', 's', 'o', 'l', 'e'};
    register_value.uri = {uri, sizeof(uri)};
    register_value.service.value = 0;

    std::uint8_t buffer[512]{};
    std::uint64_t written = 0;
    assert(encode_register_request(buffer, sizeof(buffer), register_value, written));

    register_request decoded{};
    assert(decode_register_request(buffer, written, decoded));
    assert(decoded.uri.size == sizeof(uri));
    assert(decoded.uri.data[0] == 'n');
    assert(decoded.service.value == 0);

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
