#include <cassert>
#include <cstdint>

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

    register_request register_value{};
    const std::uint8_t name[] = {'c', 'o', 'n', 's', 'o', 'l', 'e'};
    register_value.name = {name, sizeof(name)};
    register_value.service.value = 0;

    std::uint8_t buffer[512]{};
    std::uint64_t written = 0;
    assert(encode_register_request(buffer, sizeof(buffer), register_value, written));

    register_request decoded{};
    assert(decode_register_request(buffer, written, decoded));
    assert(decoded.name.size == sizeof(name));
    assert(decoded.name.data[0] == 'c');
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
