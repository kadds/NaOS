#include <cstdint>

#include "catch2_compat.hpp"
#include <naos/generated/internal_uapi.h>
#include <naos/generated/system/BlockDevice.hpp>
#include <naos/generated/system/BlockDeviceFactory.hpp>
#include <naos/generated/system/Directory.hpp>
#include <naos/generated/system/File.hpp>
#include <naos/generated/system/Framebuffer.hpp>
#include <naos/generated/system/InputEventSource.hpp>
#include <naos/generated/system/MemoryObject.hpp>
#include <naos/generated/system/MountTicket.hpp>
#include <naos/generated/system/Process.hpp>
#include <naos/generated/system/ServiceDirectory.hpp>
#include <naos/generated/system/Stream.hpp>
#include <naos/generated/system/TerminalDriverControl.hpp>
#include <naos/generated/system/TerminalDriverFactory.hpp>
#include <naos/generated/system/TerminalJobControl.hpp>
#include <naos/generated/system/TerminalManager.hpp>
#include <naos/generated/system/TerminalMaster.hpp>
#include <naos/generated/system/TerminalSlave.hpp>
#include <naos/generated/system/Vfs.hpp>
#include <naos/generated/system_uapi.h>

template <typename T>
concept has_legacy_job_control_process_id = requires(T value) { value.process_id; };

static_assert(NA_SCOPE_STREAM == 1);
static_assert(NA_SCOPE_FILE == 2);
static_assert(NA_SCOPE_DIRECTORY == 3);
static_assert(NA_SCOPE_MEMORY_OBJECT == 7);
static_assert(NA_SCOPE_PROCESS == 9);
static_assert(NA_SCOPE_TERMINAL_MANAGER == 5);
static_assert(NA_SCOPE_TERMINAL_MASTER == 6);
static_assert(NA_SCOPE_VFS == 16);
static_assert(NA_SCOPE_BLOCK_DEVICE == 17);
static_assert(NA_SCOPE_NAMESPACE_BINDING == 18);
static_assert(NA_SCOPE_MOUNT_TICKET == 19);
static_assert(NA_SCOPE_BLOCK_DEVICE_FACTORY == 20);
static_assert(NA_SCOPE_MOUNT_CONTROL == 21);
static_assert(NA_SCOPE_MUTATION_TICKET == 22);
static_assert(NA_SCOPE_FRAMEBUFFER == 23);
static_assert(NA_SCOPE_TERMINAL_SLAVE == 11);
static_assert(NA_SCOPE_TERMINAL_JOB_CONTROL == 12);
static_assert(NA_SCOPE_TERMINAL_DRIVER_CONTROL == 13);
static_assert(NA_SCOPE_TERMINAL_DRIVER_FACTORY == 14);
static_assert(NA_SCOPE_INPUT_EVENT_SOURCE == 15);
static_assert(NA_METHOD_STREAM_READ == 1);
static_assert(NA_METHOD_STREAM_WRITE == 2);
static_assert(NA_METHOD_DIRECTORY_OPEN == 1);
static_assert(NA_METHOD_DIRECTORY_CLONE_BINDING == 14);
static_assert(NA_METHOD_DIRECTORY_STAT_NODE == 15);
static_assert(NA_METHOD_DIRECTORY_SYNC == 16);
static_assert(NA_METHOD_DIRECTORY_RENAME_AT == 17);
static_assert(NA_METHOD_DIRECTORY_LINK_AT == 18);
// Reserved ordinal persisted in the manifest; never reuse method ID 19.
static_assert(NA_METHOD_DIRECTORY_SET_METADATA == 19);
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
static_assert(NA_METHOD_FILE_MATERIALIZE == 17);
static_assert(NA_METHOD_VFS_GET_ROOT == 1);
static_assert(NA_METHOD_VFS_PREPARE_MOUNT == 2);
static_assert(NA_METHOD_VFS_UNMOUNT == 3);
static_assert(NA_METHOD_VFS_SYNC == 4);
static_assert(NA_METHOD_VFS_GET_MOUNT_INFO == 5);
static_assert(NA_METHOD_BLOCK_DEVICE_GET_INFO == 1);
static_assert(NA_METHOD_BLOCK_DEVICE_READ == 2);
static_assert(NA_METHOD_BLOCK_DEVICE_WRITE == 3);
static_assert(NA_METHOD_BLOCK_DEVICE_FLUSH == 4);
static_assert(NA_METHOD_BLOCK_DEVICE_DISCARD == 5);
static_assert(NA_METHOD_BLOCK_DEVICE_FACTORY_GET_INFO == 1);
static_assert(NA_METHOD_BLOCK_DEVICE_FACTORY_ACQUIRE == 2);
static_assert(NA_METHOD_MOUNT_TICKET_COMMIT == 1);
static_assert(NA_METHOD_MOUNT_TICKET_ABORT == 2);
static_assert(NA_METHOD_MOUNT_TICKET_STATUS == 3);
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
static_assert(NA_METHOD_FRAMEBUFFER_GET == 1);

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
    // BlockDevice methods are individually authorized by the frozen
    // per-protocol named rights; get_info carries only block_inspect.
    static_assert(naos::system::BlockDevice::descriptor.method_rights[0] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_INSPECT));
    static_assert(naos::system::BlockDevice::descriptor.method_rights[1] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_READ));
    static_assert(naos::system::BlockDevice::descriptor.method_rights[2] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_WRITE));
    static_assert(naos::system::BlockDevice::descriptor.method_rights[3] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_FLUSH));
    static_assert(naos::system::BlockDevice::descriptor.method_rights[4] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_RIGHT_DISCARD));
    static_assert(naos::system::BlockDeviceFactory::descriptor.method_rights[1] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_BLOCK_DEVICE_FACTORY_RIGHT_ACQUIRE));
    static_assert(naos::system::Framebuffer::descriptor.method_rights[0] ==
                  (NA_PROTOCOL_RIGHT_INVOKE | NA_DISPLAY_RIGHT_WRITER));

    // Generated enum decoders are closed: an unknown wire value must not be
    // interpreted as the first action by a handler switch.
    const std::uint8_t unknown_driver_action[] = {0xff, 0, 0, 0};
    naos::system::TerminalDriverControl::raise_foreground_request invalid_driver_request{};
    REQUIRE(!naos::system::TerminalDriverControl::decode_raise_foreground_request(
        unknown_driver_action, sizeof(unknown_driver_action), invalid_driver_request));

    std::uint8_t buffer[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint64_t written = 0;

    const std::uint8_t payload[] = {1, 2, 3};
    // Stream payloads travel through the caller's MemoryObject region; the
    // request wire only names the region window and the requested size.
    naos::system::Stream::write_request write_request{};
    write_request.size = sizeof(payload);
    write_request.flags = 0;
    write_request.buffer.value = 0;
    REQUIRE(naos::system::Stream::encode_write_request(buffer, sizeof(buffer), write_request, written));
    REQUIRE(written == 20);
    naos::system::Stream::write_request decoded_write{};
    REQUIRE(naos::system::Stream::decode_write_request(buffer, written, decoded_write));
    REQUIRE(decoded_write.size == sizeof(payload));
    REQUIRE(decoded_write.flags == 0);
    REQUIRE(decoded_write.buffer.value == 0);

    naos::system::Stream::read_response stream_read{};
    stream_read.count = sizeof(payload);
    REQUIRE(naos::system::Stream::encode_read_response(buffer, sizeof(buffer), stream_read, written));
    REQUIRE(written == 8);
    naos::system::Stream::read_response decoded_stream_read{};
    REQUIRE(naos::system::Stream::decode_read_response(buffer, written, decoded_stream_read));
    REQUIRE(decoded_stream_read.count == sizeof(payload));

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
    file_read_request.buffer.value = 0;
    REQUIRE(naos::system::File::encode_read_request(buffer, sizeof(buffer), file_read_request, written));
    REQUIRE(written == 20);
    naos::system::File::read_request decoded_file_read{};
    REQUIRE(naos::system::File::decode_read_request(buffer, written, decoded_file_read));
    REQUIRE(decoded_file_read.size == sizeof(payload));
    REQUIRE(decoded_file_read.buffer.value == 0);

    // pread carries the resource slot alongside its offset/flags; the byte
    // count matches the generated header layout.
    naos::system::File::pread_request pread_request{};
    pread_request.offset = 128;
    pread_request.size = sizeof(payload);
    pread_request.flags = 0;
    pread_request.buffer.value = 0;
    REQUIRE(naos::system::File::encode_pread_request(buffer, sizeof(buffer), pread_request, written));
    REQUIRE(written == 28);
    naos::system::File::pread_request decoded_pread{};
    REQUIRE(naos::system::File::decode_pread_request(buffer, written, decoded_pread));
    REQUIRE(decoded_pread.offset == 128);
    REQUIRE(decoded_pread.size == sizeof(payload));
    REQUIRE(decoded_pread.buffer.value == 0);
    naos::system::File::pread_response pread_response{};
    pread_response.count = sizeof(payload);
    REQUIRE(naos::system::File::encode_pread_response(buffer, sizeof(buffer), pread_response, written));
    REQUIRE(written == 8);
    naos::system::File::pread_response decoded_pread_response{};
    REQUIRE(naos::system::File::decode_pread_response(buffer, written, decoded_pread_response));
    REQUIRE(decoded_pread_response.count == sizeof(payload));

    // getcwd() uses the empty Directory::PATH request. Keep its wire shape
    // covered as well, since the native ABI requires nullptr for zero bytes.
    naos::system::Directory::path_request path_request{};
    REQUIRE(naos::system::Directory::encode_path_request(buffer, sizeof(buffer), path_request, written));
    REQUIRE(written == 0);
    REQUIRE(naos::system::Directory::decode_path_request(nullptr, 0, path_request));

    // Framebuffer::get has the same zero-field request shape.  The kernel
    // dispatcher invokes this generated decoder, so malformed non-empty
    // payloads must be rejected instead of being treated as a valid get.
    naos::system::Framebuffer::get_request framebuffer_get{};
    REQUIRE(naos::system::Framebuffer::encode_get_request(nullptr, 0, framebuffer_get, written));
    REQUIRE(written == 0);
    REQUIRE(naos::system::Framebuffer::decode_get_request(nullptr, 0, framebuffer_get));
    const std::uint8_t extra_framebuffer_byte = 0;
    REQUIRE_FALSE(naos::system::Framebuffer::decode_get_request(&extra_framebuffer_byte, sizeof(extra_framebuffer_byte),
                                                                framebuffer_get));
    REQUIRE_FALSE(naos::system::Framebuffer::decode_get_request(nullptr, 1, framebuffer_get));

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

    // MemoryObject payloads moved into a sibling region on the same object;
    // both directions name the window and reply the transferred byte count.
    naos::system::MemoryObject::write_request memory_write{};
    memory_write.offset = 4096;
    memory_write.size = sizeof(payload);
    memory_write.buffer.value = 0;
    REQUIRE(naos::system::MemoryObject::encode_write_request(buffer, sizeof(buffer), memory_write, written));
    REQUIRE(written == 20);

    naos::system::MemoryObject::write_request decoded_memory_write{};
    REQUIRE(naos::system::MemoryObject::decode_write_request(buffer, written, decoded_memory_write));
    REQUIRE(decoded_memory_write.offset == 4096);
    REQUIRE(decoded_memory_write.size == sizeof(payload));
    REQUIRE(decoded_memory_write.buffer.value == 0);

    naos::system::MemoryObject::read_request memory_read{};
    memory_read.offset = 8192;
    memory_read.size = sizeof(payload);
    memory_read.buffer.value = 0;
    REQUIRE(naos::system::MemoryObject::encode_read_request(buffer, sizeof(buffer), memory_read, written));
    REQUIRE(written == 20);
    naos::system::MemoryObject::read_request decoded_memory_read{};
    REQUIRE(naos::system::MemoryObject::decode_read_request(buffer, written, decoded_memory_read));
    REQUIRE(decoded_memory_read.offset == 8192);
    REQUIRE(decoded_memory_read.size == sizeof(payload));
    naos::system::MemoryObject::read_response memory_read_response{};
    memory_read_response.count = sizeof(payload);
    REQUIRE(naos::system::MemoryObject::encode_read_response(buffer, sizeof(buffer), memory_read_response, written));
    REQUIRE(written == 8);
    naos::system::MemoryObject::read_response decoded_memory_read_response{};
    REQUIRE(naos::system::MemoryObject::decode_read_response(buffer, written, decoded_memory_read_response));
    REQUIRE(decoded_memory_read_response.count == sizeof(payload));

    // stat_node replaces the open+stat simulation.
    naos::system::Directory::stat_node_request stat_node_request{};
    stat_node_request.flags = 1; // NA_DIRECTORY_LOOKUP_FLAG_NOFOLLOW
    const std::uint8_t node_path[] = {'a', 'b'};
    stat_node_request.path_size = sizeof(node_path);
    stat_node_request.path = {node_path, sizeof(node_path)};
    REQUIRE(naos::system::Directory::encode_stat_node_request(buffer, sizeof(buffer), stat_node_request, written));
    REQUIRE(written == 18);
    naos::system::Directory::stat_node_request decoded_stat_node{};
    REQUIRE(naos::system::Directory::decode_stat_node_request(buffer, written, decoded_stat_node));
    REQUIRE(decoded_stat_node.flags == 1);
    REQUIRE(decoded_stat_node.path.size == sizeof(node_path));

    // Directory::list records now land in the caller's region; the request
    // names the window and the response reports records/bytes written, not
    // the record bytes themselves.
    naos::system::Directory::list_request directory_list{};
    directory_list.offset = 0;
    directory_list.requested_bytes = 4096;
    directory_list.buffer.value = 0;
    REQUIRE(naos::system::Directory::encode_list_request(buffer, sizeof(buffer), directory_list, written));
    REQUIRE(written == 20);
    naos::system::Directory::list_request decoded_directory_list{};
    REQUIRE(naos::system::Directory::decode_list_request(buffer, written, decoded_directory_list));
    REQUIRE(decoded_directory_list.offset == 0);
    REQUIRE(decoded_directory_list.requested_bytes == 4096);
    REQUIRE(decoded_directory_list.buffer.value == 0);
    naos::system::Directory::list_response directory_list_response{};
    directory_list_response.next = 2;
    directory_list_response.count = 3;
    directory_list_response.bytes = 96;
    REQUIRE(naos::system::Directory::encode_list_response(buffer, sizeof(buffer), directory_list_response, written));
    REQUIRE(written == 24);
    naos::system::Directory::list_response decoded_directory_list_response{};
    REQUIRE(naos::system::Directory::decode_list_response(buffer, written, decoded_directory_list_response));
    REQUIRE(decoded_directory_list_response.next == 2);
    REQUIRE(decoded_directory_list_response.count == 3);
    REQUIRE(decoded_directory_list_response.bytes == 96);

    // File materialize returns a move-only MemoryObject handle plus the
    // snapshot length/generation (handle travels in the resource table).
    naos::system::File::materialize_response materialize_response{};
    materialize_response.length = 4096;
    materialize_response.generation = 7;
    REQUIRE(naos::system::File::encode_materialize_response(buffer, sizeof(buffer), materialize_response, written));
    REQUIRE(written == 20);
    REQUIRE(naos::system::File::decode_materialize_response(buffer, written, materialize_response));
    REQUIRE(materialize_response.length == 4096);
    REQUIRE(materialize_response.generation == 7);
}
