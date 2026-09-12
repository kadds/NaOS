#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "catch2_compat.hpp"
#include <naos/generated/rust_interop/Echo.hpp>
#include <naos/generated/rust_interop/Echo_server.hpp>

#ifndef NAOS_RUST_INTEROP_BINARY
#error "NAOS_RUST_INTEROP_BINARY must point to the Rust fixture"
#endif

namespace
{
using naos_test::Echo::echo_request;
using naos_test::Echo::echo_response;

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

std::string shell_quote(const std::string &value)
{
    std::string quoted{"'"};
    for (const char character : value)
    {
        if (character == '\'')
            quoted += "'\\''";
        else
            quoted += character;
    }
    quoted += '\'';
    return quoted;
}

void run_fixture(std::string_view command, const std::filesystem::path &wire)
{
    const std::string invocation =
        shell_quote(NAOS_RUST_INTEROP_BINARY) + " " + std::string(command) + " " + shell_quote(wire.string());
    REQUIRE(std::system(invocation.c_str()) == 0);
}

struct reply_capture
{
    std::vector<std::uint8_t> wire;
};

na_status_t reply(void *context, na_handle_t, const std::uint8_t *wire, std::uint64_t bytes,
                  const na_resource_disposition_t *, std::uint64_t)
{
    auto &capture = *static_cast<reply_capture *>(context);
    capture.wire.assign(wire, wire + bytes);
    return NA_STATUS_OK;
}

na_status_t fail(void *, na_handle_t, na_execution_outcome_t, na_outcome_reason_t, std::int64_t)
{
    return NA_STATUS_IO_ERROR;
}

na_status_t close_endpoint(void *, na_handle_t) { return NA_STATUS_OK; }

struct echo_handler
{
    void set_dispatch_context(na_handle_t, na_resource_disposition_t *, std::uint64_t, const na_handle_t *,
                              std::uint64_t, std::uint64_t)
    {
    }

    bool echo(const echo_request &request, echo_response &response)
    {
        response.payload = request.payload;
        return true;
    }

    naoidl::dispatch_outcome dispatch_outcome() const { return naoidl::dispatch_outcome::failed; }
    na_outcome_reason_t dispatch_failure_reason() const { return NA_OUTCOME_REASON_NONE; }
    std::int64_t dispatch_failure_error() const { return 0; }
    void rollback_dispatch() {}
    void commit_dispatch() {}
};
} // namespace

TEST_CASE("C++ and Rust Echo bindings round-trip in both directions", "[interop]")
{
    const auto temporary_directory = std::filesystem::temp_directory_path() /
                                     ("naos-interop-echo-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(temporary_directory);
    const auto cleanup = [&] {
        std::error_code error;
        std::filesystem::remove_all(temporary_directory, error);
    };

    const auto rust_request_wire = temporary_directory / "rust-request.bin";
    const auto cxx_request_wire = temporary_directory / "cxx-request.bin";
    const auto cxx_response_wire = temporary_directory / "cxx-response.bin";

    // Rust client -> C++ decoder.
    run_fixture("encode-request", rust_request_wire);
    const auto rust_wire = read_bytes(rust_request_wire);
    echo_request rust_request{};
    REQUIRE(naos_test::Echo::decode_echo_request(rust_wire.data(), rust_wire.size(), rust_request));
    REQUIRE(
        std::vector<std::uint8_t>(rust_request.payload.data, rust_request.payload.data + rust_request.payload.size) ==
        std::vector<std::uint8_t>{'r', 'u', 's', 't', '-', 'c', 'x', 'x', '-', 'i', 'n', 't', 'e', 'r', 'o', 'p'});

    // C++ client -> Rust decoder.
    const std::vector<std::uint8_t> cxx_payload{'c', 'x', 'x', '-', 'r', 'u', 's', 't',
                                                '-', 'i', 'n', 't', 'e', 'r', 'o', 'p'};
    echo_request cxx_request{naoidl::bounded_bytes{cxx_payload.data(), static_cast<std::uint32_t>(cxx_payload.size())}};
    std::vector<std::uint8_t> cxx_wire(128);
    std::uint64_t cxx_wire_size = 0;
    REQUIRE(naos_test::Echo::encode_echo_request(cxx_wire.data(), cxx_wire.size(), cxx_request, cxx_wire_size));
    cxx_wire.resize(cxx_wire_size);
    write_bytes(cxx_request_wire, cxx_wire);
    run_fixture("decode-request", cxx_request_wire);

    // C++ generated server dispatch -> Rust decoder.
    reply_capture capture{};
    naoidl::responder_transport transport{};
    transport.context = &capture;
    transport.reply = reply;
    transport.fail = fail;
    transport.close_endpoint = close_endpoint;
    echo_handler handler{};
    std::uint8_t response_buffer[128]{};
    REQUIRE(naos_test::Echo::EchoServer::dispatch(
                handler, transport, NA_HANDLE_INVALID, NA_HANDLE_INVALID, naos_test::Echo::method_echo, cxx_wire.data(),
                cxx_wire.size(), nullptr, 0, response_buffer, sizeof(response_buffer), nullptr, 0, 0) == NA_STATUS_OK);
    REQUIRE(capture.wire == cxx_wire);
    write_bytes(cxx_response_wire, capture.wire);
    run_fixture("decode-response", cxx_response_wire);

    cleanup();
}
