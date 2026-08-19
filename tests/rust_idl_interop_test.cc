#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include "catch2_compat.hpp"
#include <naos/generated/rust_interop/Echo.hpp>

TEST_CASE("Rust NaoIDL bytes decode with the C++ binding", "[rust-idl]")
{
    std::ifstream input(NAOS_RUST_IDL_FIXTURE, std::ios::binary);
    REQUIRE(input.good());
    const std::vector<std::uint8_t> wire((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    naos_test::Echo::echo_request request{};
    REQUIRE(naos_test::Echo::decode_echo_request(wire.data(), wire.size(), request));
    REQUIRE(request.payload.size == 16);
    REQUIRE(
        std::vector<std::uint8_t>(request.payload.data, request.payload.data + request.payload.size) ==
        std::vector<std::uint8_t>({'r', 'u', 's', 't', '-', 'c', 'x', 'x', '-', 'i', 'n', 't', 'e', 'r', 'o', 'p'}));

    std::uint8_t response_wire[128]{};
    naos_test::Echo::echo_response response{};
    response.payload = request.payload;
    std::uint64_t written = 0;
    REQUIRE(naos_test::Echo::encode_echo_response(response_wire, sizeof(response_wire), response, written));
    REQUIRE(written == wire.size());
}
