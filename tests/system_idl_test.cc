#include <cstdint>

#include "catch2_compat.hpp"
#include <naos/generated/system/Directory.hpp>
#include <naos/generated/system/File.hpp>
#include <naos/generated/system/InputEventSource.hpp>
#include <naos/generated/system/MemoryObject.hpp>
#include <naos/generated/system/Process.hpp>
#include <naos/generated/system/SharedRing.hpp>
#include <naos/generated/system/Stream.hpp>
#include <naos/generated/system/TerminalDriverControl.hpp>
#include <naos/generated/system/TerminalDriverFactory.hpp>
#include <naos/generated/system/TerminalJobControl.hpp>
#include <naos/generated/system/TerminalManager.hpp>
#include <naos/generated/system/TerminalMaster.hpp>
#include <naos/generated/system/TerminalSlave.hpp>
#include <naos/generated/system_uapi.h>

template <typename T>
concept has_legacy_job_control_process_id = requires(T value) { value.process_id; };

static_assert(NA_SCOPE_STREAM == 1);
static_assert(NA_SCOPE_FILE == 2);
static_assert(NA_SCOPE_DIRECTORY == 3);
static_assert(NA_SCOPE_MEMORY_OBJECT == 7);
static_assert(NA_SCOPE_SHARED_RING == 8);
static_assert(NA_SCOPE_PROCESS == 9);
static_assert(NA_SCOPE_TERMINAL_MANAGER == 5);
static_assert(NA_SCOPE_TERMINAL_MASTER == 6);
static_assert(NA_SCOPE_TERMINAL_SLAVE == 11);
static_assert(NA_SCOPE_TERMINAL_JOB_CONTROL == 12);
static_assert(NA_SCOPE_TERMINAL_DRIVER_CONTROL == 13);
static_assert(NA_SCOPE_TERMINAL_DRIVER_FACTORY == 14);
static_assert(NA_SCOPE_INPUT_EVENT_SOURCE == 15);
static_assert(NA_METHOD_STREAM_READ == 1);
static_assert(NA_METHOD_STREAM_WRITE == 2);
static_assert(NA_METHOD_DIRECTORY_OPEN == 1);
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
static_assert(NA_METHOD_MEMORY_OBJECT_GET_INFO == 1);
static_assert(NA_METHOD_MEMORY_OBJECT_READ == 2);
static_assert(NA_METHOD_MEMORY_OBJECT_WRITE == 3);
static_assert(NA_METHOD_SHARED_RING_GET_INFO == 1);
static_assert(NA_METHOD_SHARED_RING_PUSH == 2);
static_assert(NA_METHOD_SHARED_RING_POP == 3);

TEST_CASE("generated system IDL contract", "[system-idl]")
{
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

    // Generated enum decoders are closed: an unknown wire value must not be
    // interpreted as the first action by a handler switch.
    const std::uint8_t unknown_driver_action[] = {0xff, 0, 0, 0};
    naos::system::TerminalDriverControl::raise_foreground_request invalid_driver_request{};
    REQUIRE(!naos::system::TerminalDriverControl::decode_raise_foreground_request(
        unknown_driver_action, sizeof(unknown_driver_action), invalid_driver_request));

    std::uint8_t buffer[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;

    const std::uint8_t payload[] = {1, 2, 3};
    naos::system::Stream::write_request write_request{};
    write_request.size = sizeof(payload);
    write_request.data = {payload, sizeof(payload)};
    REQUIRE(naos::system::Stream::encode_write_request(buffer, sizeof(buffer), write_request, written));
    REQUIRE(written == 19);
    naos::system::Stream::write_request decoded_write{};
    REQUIRE(naos::system::Stream::decode_write_request(buffer, written, decoded_write));
    REQUIRE(decoded_write.size == sizeof(payload));
    REQUIRE(decoded_write.data.size == sizeof(payload));
    REQUIRE(decoded_write.data.data[2] == payload[2]);

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
    REQUIRE(naos::system::File::encode_read_request(buffer, sizeof(buffer), file_read_request, written));
    REQUIRE(written == 16);
    naos::system::File::read_request decoded_file_read{};
    REQUIRE(naos::system::File::decode_read_request(buffer, written, decoded_file_read));
    REQUIRE(decoded_file_read.size == sizeof(payload));

    // getcwd() uses the empty Directory::PATH request. Keep its wire shape
    // covered as well, since the native ABI requires nullptr for zero bytes.
    naos::system::Directory::path_request path_request{};
    REQUIRE(naos::system::Directory::encode_path_request(buffer, sizeof(buffer), path_request, written));
    REQUIRE(written == 0);
    REQUIRE(naos::system::Directory::decode_path_request(nullptr, 0, path_request));

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
    REQUIRE(naos::system::Process::encode_wait_response(buffer, sizeof(buffer), wait_response, written));
    REQUIRE(written == 16);
    naos::system::Process::wait_response decoded_wait{};
    REQUIRE(naos::system::Process::decode_wait_response(buffer, written, decoded_wait));
    REQUIRE(decoded_wait.status == -1);
    REQUIRE(decoded_wait.pid == 42);

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

    naos::system::MemoryObject::write_request memory_write{};
    memory_write.offset = 4096;
    memory_write.size = sizeof(payload);
    memory_write.data = {payload, sizeof(payload)};
    REQUIRE(naos::system::MemoryObject::encode_write_request(buffer, sizeof(buffer), memory_write, written));
    REQUIRE(written == 19);
    naos::system::MemoryObject::write_request decoded_memory_write{};
    REQUIRE(naos::system::MemoryObject::decode_write_request(buffer, written, decoded_memory_write));
    REQUIRE(decoded_memory_write.offset == 4096);
    REQUIRE(decoded_memory_write.size == sizeof(payload));
    REQUIRE(decoded_memory_write.data.size == sizeof(payload));
    REQUIRE(decoded_memory_write.data.data[2] == payload[2]);

    naos::system::SharedRing::pop_response ring_pop{};
    ring_pop.required_bytes = 3;
    ring_pop.data = {payload, sizeof(payload)};
    REQUIRE(naos::system::SharedRing::encode_pop_response(buffer, sizeof(buffer), ring_pop, written));
    REQUIRE(written == 11);
    naos::system::SharedRing::pop_response decoded_ring_pop{};
    REQUIRE(naos::system::SharedRing::decode_pop_response(buffer, written, decoded_ring_pop));
    REQUIRE(decoded_ring_pop.required_bytes == sizeof(payload));
    REQUIRE(decoded_ring_pop.data.size == sizeof(payload));
    REQUIRE(decoded_ring_pop.data.data[2] == payload[2]);
}
