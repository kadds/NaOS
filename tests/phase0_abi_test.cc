#include "naos/abi.h"
#include "naos/bootstrap.hpp"
#include "naos/canonical.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
constexpr bool syscall_numbers_are_dense()
{
    constexpr std::array<std::uint32_t, 41> numbers = {
        NA_SYSCALL_LOG,
        NA_SYSCALL_CLOCK_GET,
        NA_SYSCALL_FUTEX,
        NA_SYSCALL_EXIT,
        NA_SYSCALL_EXIT_THREAD,
        NA_SYSCALL_SLEEP,
        NA_SYSCALL_CURRENT_PID,
        NA_SYSCALL_CURRENT_TID,
        NA_SYSCALL_SIGSEND,
        NA_SYSCALL_SIGMASK,
        NA_SYSCALL_SET_TCB,
        NA_SYSCALL_FORK,
        NA_SYSCALL_CLONE,
        NA_SYSCALL_YIELD,
        NA_SYSCALL_BRK,
        NA_SYSCALL_SBRK,
        NA_SYSCALL_HANDLE_CLOSE,
        NA_SYSCALL_CHANNEL_CREATE,
        NA_SYSCALL_CHANNEL_SEND,
        NA_SYSCALL_CHANNEL_RECEIVE,
        NA_SYSCALL_CHANNEL_DISCARD,
        NA_SYSCALL_HANDLE_WAIT_MANY,
        NA_SYSCALL_HANDLE_DUPLICATE,
        NA_SYSCALL_HANDLE_RESTRICT,
        NA_SYSCALL_HANDLE_GET_INFO,
        NA_SYSCALL_PROTOCOL_DESCRIPTOR_CREATE,
        NA_SYSCALL_PROTOCOL_ENDPOINT_CREATE,
        NA_SYSCALL_INVOKE_SUBMIT,
        NA_SYSCALL_INVOKE_SEND_ONEWAY,
        NA_SYSCALL_INVOCATION_CANCEL,
        NA_SYSCALL_INVOCATION_TAKE_RESULT,
        NA_SYSCALL_RESPONDER_REPLY,
        NA_SYSCALL_RESPONDER_FAIL,
        NA_SYSCALL_BOOTSTRAP,
        NA_SYSCALL_TTY_CONTROL_ACQUIRE,
        NA_SYSCALL_MEMORY_MAP,
        NA_SYSCALL_MEMORY_UNMAP,
        NA_SYSCALL_PROCESS_EXEC,
        NA_SYSCALL_PROCESS_HANDLE_OPEN,
        NA_SYSCALL_PROCESS_SPAWN,
        NA_SYSCALL_PIPE_CREATE,
    };
    for (std::uint32_t index = 0; index < numbers.size(); index++)
    {
        const auto expected = index + 1;
        if (numbers[index] != expected)
            return false;
    }
    return true;
}

void test_layouts()
{
    static_assert(sizeof(na_handle_t) == 8);
    static_assert(sizeof(na_uuid_t) == 16);
    static_assert(sizeof(na_resource_disposition_t) == 32);
    static_assert(sizeof(na_submit_frame_t) == 72);
    static_assert(sizeof(na_channel_receive_frame_t) == 96);
    static_assert(sizeof(na_result_frame_t) == 104);
    static_assert(sizeof(na_bootstrap_message_t) == 80);
    static_assert(sizeof(na_process_spawn_frame_t) == 80);
    static_assert(offsetof(na_submit_frame_t, method_id) == 8);
    static_assert(offsetof(na_submit_frame_t, resources) == 32);
    static_assert(offsetof(na_result_frame_t, execution_outcome) == 80);
    static_assert(NA_CHANNEL_MAX_MESSAGE_BYTES == 65536);
    static_assert(NA_CHANNEL_MAX_RESOURCES == 64);
    static_assert(NA_HANDLE_INVALID == 0);
    static_assert(NA_SYSCALL_NONE == 0);
    static_assert(NA_SYSCALL_COUNT == 42);
    static_assert(NA_SYSCALL_MEMORY_MAP == 36);
    static_assert(NA_SYSCALL_PROCESS_SPAWN == 40);
    static_assert(syscall_numbers_are_dense());
}

void test_canonical_little_endian()
{
    std::array<std::uint8_t, 32> bytes{};
    naos::canonical::writer writer(bytes.data(), bytes.size());
    writer.put_u16(0x1234);
    writer.put_u32(0x78563412);
    writer.put_u64(0xEFCDAB8967452301ULL);
    assert(writer.good());
    assert(writer.size() == 14);
    const std::array<std::uint8_t, 14> expected = {0x34, 0x12, 0x12, 0x34, 0x56, 0x78, 0x01,
                                                   0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    assert(std::memcmp(bytes.data(), expected.data(), expected.size()) == 0);

    naos::canonical::reader reader(bytes.data(), writer.size());
    assert(reader.get_u16() == 0x1234);
    assert(reader.get_u32() == 0x78563412);
    assert(reader.get_u64() == 0xEFCDAB8967452301ULL);
    assert(reader.good());
    assert(reader.remaining() == 0);
}

void test_empty_value()
{
    naos::canonical::writer writer(nullptr, 0);
    writer.put_bytes(nullptr, 0);
    assert(writer.good());
    assert(writer.size() == 0);
    naos::canonical::reader reader(nullptr, 0);
    assert(reader.get_bytes(nullptr, 0));
    assert(reader.good());
}
} // namespace

int main()
{
    test_layouts();
    test_canonical_little_endian();
    test_empty_value();
    return 0;
}
