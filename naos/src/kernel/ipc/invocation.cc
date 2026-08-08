#include "kernel/ipc/invocation.hpp"
#include "kernel/ipc/channel.hpp"

#include "kernel/arch/klib.hpp"
#include "kernel/dev/tty/pty.hpp"
#include "kernel/dev/tty/pty_manager.hpp"
#include "kernel/dev/tty/tty.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/stat.hpp"
#include "kernel/fs/vfs/file.hpp"
#include "kernel/fs/vfs/inode.hpp"
#include "kernel/fs/vfs/native_directory.hpp"
#include "kernel/fs/vfs/vfs.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/service_directory.hpp"
#include "kernel/task.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include "naos/canonical.hpp"
#include "naos/generated/system/Directory.hpp"
#include "naos/generated/system/File.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include "naos/generated/system/Process.hpp"
#include "naos/generated/system/SharedRing.hpp"
#include "naos/generated/system/ServiceDirectory.hpp"
#include "naos/generated/system/Stream.hpp"
#include "naos/generated/system/TtyControl.hpp"
#include "naos/generated/system_uapi.h"
#include <limits>

namespace naos::ipc
{
namespace
{
constexpr u64 max_kernel_payload = NA_CHANNEL_MAX_MESSAGE_BYTES;
constexpr u64 no_deadline = 0;

struct deadline_watch
{
    handle_t<invocation_state> state;

    explicit deadline_watch(const handle_t<invocation_state> &state)
        : state(state)
    {
    }

    void invoke(timeclock::microsecond_t) noexcept
    {
        if (state)
            state->expire_deadline();
        state.reset();
        memory::Delete<>(memory::KernelCommonAllocatorV, this);
    }
};

std::atomic_uint64_t global_protocol_messages{0};
std::atomic_uint64_t global_protocol_bytes{0};
std::atomic_uint64_t global_protocol_resources{0};

bool reserve_protocol_global(u64 bytes, u64 resources)
{
    auto reserve = [](std::atomic_uint64_t &counter, u64 amount, u64 limit) {
        if (amount > limit)
            return false;
        auto current = counter.load(std::memory_order_acquire);
        for (;;)
        {
            if (current > limit - amount)
                return false;
            if (counter.compare_exchange_weak(current, current + amount, std::memory_order_acq_rel))
                return true;
        }
    };
    if (!reserve(global_protocol_messages, 1, NA_CHANNEL_GLOBAL_MAX_MESSAGES))
        return false;
    if (!reserve(global_protocol_bytes, bytes, NA_CHANNEL_GLOBAL_MAX_BYTES))
    {
        global_protocol_messages.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    if (!reserve(global_protocol_resources, resources, NA_CHANNEL_GLOBAL_MAX_RESOURCES))
    {
        global_protocol_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
        global_protocol_messages.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

void release_protocol_global(u64 bytes, u64 resources)
{
    global_protocol_messages.fetch_sub(1, std::memory_order_acq_rel);
    global_protocol_bytes.fetch_sub(bytes, std::memory_order_acq_rel);
    global_protocol_resources.fetch_sub(resources, std::memory_order_acq_rel);
}

void wake_invocation_waiters() { notify_channel_waiters(); }

freelibcxx::vector<byte> empty_bytes() { return freelibcxx::vector<byte>(memory::MemoryAllocatorV); }

capability::transfer_record_list empty_resources()
{
    return capability::transfer_record_list(memory::KernelCommonAllocatorV);
}

bool checked_multiply(u64 left, u64 right, u64 &result)
{
    if (left != 0 && right > std::numeric_limits<u64>::max() / left)
        return false;
    result = left * right;
    return true;
}

bool valid_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason)
{
    if ((outcome != NA_EXECUTION_NOT_DELIVERED && outcome != NA_EXECUTION_OUTCOME_UNKNOWN) ||
        reason <= NA_OUTCOME_REASON_NONE || reason > NA_OUTCOME_REASON_PROTOCOL_VIOLATION)
        return false;

    if (outcome == NA_EXECUTION_NOT_DELIVERED)
    {
        return reason == NA_OUTCOME_REASON_PEER_CLOSED || reason == NA_OUTCOME_REASON_OBJECT_REVOKED ||
               reason == NA_OUTCOME_REASON_OPERATION_DEADLINE || reason == NA_OUTCOME_REASON_CANCEL_REQUESTED ||
               reason == NA_OUTCOME_REASON_REQUEST_DISCARDED || reason == NA_OUTCOME_REASON_RESPONDER_ABANDONED ||
               reason == NA_OUTCOME_REASON_PROTOCOL_VIOLATION;
    }

    return reason == NA_OUTCOME_REASON_PEER_CLOSED || reason == NA_OUTCOME_REASON_OBJECT_REVOKED ||
           reason == NA_OUTCOME_REASON_OPERATION_DEADLINE || reason == NA_OUTCOME_REASON_CANCEL_REQUESTED ||
           reason == NA_OUTCOME_REASON_RESPONDER_ABANDONED || reason == NA_OUTCOME_REASON_BROKER_FAILURE ||
           reason == NA_OUTCOME_REASON_PROTOCOL_VIOLATION;
}

bool has_prefix(const char *value, const char *prefix)
{
    if (value == nullptr || prefix == nullptr)
        return false;
    while (*prefix != 0)
    {
        if (*value++ != *prefix++)
            return false;
    }
    return true;
}

u64 calculate_deadline(u64 budget)
{
    if (budget == 0)
        return no_deadline;
    const u64 now = timer::get_high_resolution_time();
    if (budget > std::numeric_limits<u64>::max() - now)
        return std::numeric_limits<u64>::max();
    return now + budget;
}

bool valid_frame_size(u32 actual, u64 expected) { return naos::usercopy::valid_struct_size(actual, expected); }

template <typename T> na_status_t copy_frame(T &destination, const T *source)
{
    return naos::usercopy::copy_versioned(destination, source);
}

template <typename T> na_status_t write_frame(T *destination, const T &source)
{
    return naos::usercopy::copy_to(reinterpret_cast<u64>(destination), &source, sizeof(T));
}

template <typename T> bool valid_user_output(T *output)
{
    return output != nullptr && is_user_space_range(output, sizeof(T));
}

void append_bytes(freelibcxx::vector<byte> &destination, const byte *source, u64 size)
{
    if (size == 0)
        return;
    for (u64 i = 0; i < size; i++)
        destination.push_back(source[i]);
}

template <typename Message, typename Encoder>
bool encode_message(freelibcxx::vector<byte> &destination, const Message &message, Encoder encoder)
{
    destination.resize(max_kernel_payload, byte{});
    u64 written = 0;
    if (!encoder(reinterpret_cast<u8 *>(destination.data()), destination.size(), message, written))
    {
        destination.clear();
        return false;
    }
    destination.resize(written, byte{});
    return true;
}

template <typename Message, typename Decoder>
bool decode_message(const freelibcxx::vector<byte> &source, Message &message, Decoder decoder)
{
    return decoder(reinterpret_cast<const u8 *>(source.data()), source.size(), message);
}

void append_u64(freelibcxx::vector<byte> &destination, u64 value)
{
    byte encoded[sizeof(value)];
    for (u64 i = 0; i < sizeof(value); i++)
        encoded[i] = static_cast<byte>((value >> (i * 8)) & 0xff);
    for (byte value_byte : encoded)
        destination.push_back(value_byte);
}

void append_u32(freelibcxx::vector<byte> &destination, u32 value)
{
    byte encoded[sizeof(value)];
    for (u64 i = 0; i < sizeof(value); i++)
        encoded[i] = static_cast<byte>((value >> (i * 8)) & 0xff);
    for (byte value_byte : encoded)
        destination.push_back(value_byte);
}

void append_canonical_termios(freelibcxx::vector<byte> &destination, const dev::tty::termios_t &value)
{
    naos::system::TtyControl::get_attributes_response response{};
    response.attributes.input_flags = value.c_iflag;
    response.attributes.output_flags = value.c_oflag;
    response.attributes.control_flags = value.c_cflag;
    response.attributes.local_flags = value.c_lflag;
    response.attributes.line = value.c_line;
    for (u64 i = 0; i < sizeof(value.c_cc); i++)
        response.attributes.control_chars[i] = value.c_cc[i];
    response.attributes.input_baud = value.ibaud;
    response.attributes.output_baud = value.obaud;
    encode_message(destination, response, naos::system::TtyControl::encode_get_attributes_response);
}

bool decode_canonical_termios(const freelibcxx::vector<byte> &source, dev::tty::termios_t &value)
{
    naos::system::TtyControl::set_attributes_request request{};
    if (!decode_message(source, request, naos::system::TtyControl::decode_set_attributes_request) ||
        request.attributes.padding0 != 0 || request.attributes.padding1 != 0 || request.attributes.padding2 != 0)
        return false;
    value = {};
    value.c_iflag = request.attributes.input_flags;
    value.c_oflag = request.attributes.output_flags;
    value.c_cflag = request.attributes.control_flags;
    value.c_lflag = request.attributes.local_flags;
    value.c_line = request.attributes.line;
    for (u64 i = 0; i < sizeof(value.c_cc); i++)
        value.c_cc[i] = request.attributes.control_chars[i];
    value.ibaud = request.attributes.input_baud;
    value.obaud = request.attributes.output_baud;
    return true;
}

void append_canonical_winsize(freelibcxx::vector<byte> &destination, const dev::tty::winsize_t &value)
{
    naos::system::TtyControl::get_winsize_response response{};
    response.size.rows = value.ws_row;
    response.size.columns = value.ws_col;
    response.size.x_pixels = value.ws_xpixel;
    response.size.y_pixels = value.ws_ypixel;
    encode_message(destination, response, naos::system::TtyControl::encode_get_winsize_response);
}

bool decode_canonical_winsize(const freelibcxx::vector<byte> &source, dev::tty::winsize_t &value)
{
    naos::system::TtyControl::set_winsize_request request{};
    if (!decode_message(source, request, naos::system::TtyControl::decode_set_winsize_request))
        return false;
    value.ws_row = request.size.rows;
    value.ws_col = request.size.columns;
    value.ws_xpixel = request.size.x_pixels;
    value.ws_ypixel = request.size.y_pixels;
    return true;
}

void append_canonical_stat(freelibcxx::vector<byte> &destination, const naos_stat &value)
{
    naos::system::File::stat_response response{};
    response.value.device = value.st_dev;
    response.value.inode = value.st_ino;
    response.value.links = value.st_nlink;
    response.value.mode = value.st_mode;
    response.value.uid = static_cast<u32>(value.st_uid);
    response.value.gid = static_cast<u32>(value.st_gid);
    response.value.padding = value.__pad0;
    response.value.device_id = value.st_rdev;
    response.value.size = value.st_size;
    response.value.block_size = value.st_blksize;
    response.value.blocks = value.st_blocks;
    response.value.access_seconds = value.st_atim.tv_sec;
    response.value.access_nanoseconds = value.st_atim.tv_nsec;
    response.value.modify_seconds = value.st_mtim.tv_sec;
    response.value.modify_nanoseconds = value.st_mtim.tv_nsec;
    response.value.change_seconds = value.st_ctim.tv_sec;
    response.value.change_nanoseconds = value.st_ctim.tv_nsec;
    response.value.unused0 = value.__unused[0];
    response.value.unused1 = value.__unused[1];
    response.value.unused2 = value.__unused[2];
    encode_message(destination, response, naos::system::File::encode_stat_response);
}

capability::metadata kernel_view_metadata(u64 scope, const na_uuid_t &uuid, na_meta_rights_t rights,
                                          u64 protocol_rights)
{
    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = uuid;
    metadata.scope = scope;
    metadata.revision = 1;
    metadata.meta_rights = rights;
    metadata.protocol_rights = protocol_rights | NA_PROTOCOL_RIGHT_INVOKE;
    return metadata;
}

capability::transfer_record make_response_resource(khandle object, capability::metadata metadata)
{
    capability::transferred_resource resource(std::move(object), metadata);
    return capability::transfer_record(NA_HANDLE_INVALID, true, std::move(resource));
}

na_status_t validate_submit_frame(const na_submit_frame_t &frame, bool oneway)
{
    if (!valid_frame_size(frame.struct_size, sizeof(frame)) ||
        (frame.flags & ~(NA_CALL_FLAG_ONEWAY | NA_CALL_FLAG_FLEXIBLE)) != 0 ||
        (!oneway && (frame.flags & NA_CALL_FLAG_ONEWAY) != 0) || frame.reserved0 != 0 || frame.reserved1 != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (frame.request_bytes > max_kernel_payload || frame.resource_count > NA_CHANNEL_MAX_RESOURCES)
        return NA_STATUS_INVALID_MESSAGE;
    u64 disposition_bytes = 0;
    if (!checked_multiply(frame.resource_count, sizeof(na_resource_disposition_t), disposition_bytes))
        return NA_STATUS_INVALID_ARGUMENT;
    if (!naos::usercopy::valid_range(frame.request, frame.request_bytes) ||
        !naos::usercopy::valid_range(frame.resources, disposition_bytes))
        return NA_STATUS_FAULT;
    if (frame.request_bytes != 0 && frame.request == 0)
        return NA_STATUS_FAULT;
    if (frame.resource_count != 0 && frame.resources == 0)
        return NA_STATUS_FAULT;
    return NA_STATUS_OK;
}

na_status_t snapshot_request(const na_submit_frame_t &frame, freelibcxx::vector<byte> &bytes,
                             freelibcxx::vector<na_resource_disposition_t> &dispositions)
{
    bytes.resize(frame.request_bytes, byte{});
    if (frame.request_bytes != 0)
    {
        const auto status = naos::usercopy::copy_from(bytes.data(), frame.request, frame.request_bytes);
        if (status != NA_STATUS_OK)
            return status;
    }

    if (frame.resource_count == 0)
        return NA_STATUS_OK;
    dispositions.resize(frame.resource_count, na_resource_disposition_t{});
    const u64 byte_count = frame.resource_count * sizeof(na_resource_disposition_t);
    const auto status = naos::usercopy::copy_from(dispositions.data(), frame.resources, byte_count);
    if (status != NA_STATUS_OK)
        return status;
    return NA_STATUS_OK;
}

na_status_t publish_file_call(invocation_state &state, fs::vfs::file &file, u64 scope, u64 method_id,
                              const freelibcxx::vector<byte> &request)
{
    auto *allocator = memory::MemoryAllocatorV;
    freelibcxx::vector<byte> response(allocator);
    auto *pseudo = file.get_pseudo();

    if (scope == NA_SCOPE_TTY_CONTROL && method_id >= NA_METHOD_TTY_GET_ATTRIBUTES &&
        method_id <= NA_METHOD_TTY_GET_INPUT)
    {
        if (pseudo == nullptr)
            return state.complete_reply(empty_bytes(), empty_resources(), ENOTTY) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        if (method_id == NA_METHOD_TTY_GET_ATTRIBUTES)
        {
            naos::system::TtyControl::get_attributes_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_get_attributes_request))
                return NA_STATUS_INVALID_MESSAGE;
            dev::tty::termios_t attributes{};
            if (!pseudo->native_tty_get_attributes(attributes))
                return state.complete_reply(empty_bytes(), empty_resources(), ENOTTY) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            append_canonical_termios(response, attributes);
        }
        else if (method_id == NA_METHOD_TTY_SET_ATTRIBUTES)
        {
            if (request.size() != sizeof(dev::tty::termios_t))
                return NA_STATUS_INVALID_MESSAGE;
            dev::tty::termios_t attributes{};
            if (!decode_canonical_termios(request, attributes))
                return NA_STATUS_INVALID_MESSAGE;
            if (!pseudo->native_tty_set_attributes(attributes))
                return state.complete_reply(empty_bytes(), empty_resources(), ENOTTY) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
        }
        else if (method_id == NA_METHOD_TTY_GET_WINSIZE)
        {
            naos::system::TtyControl::get_winsize_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_get_winsize_request))
                return NA_STATUS_INVALID_MESSAGE;
            dev::tty::winsize_t size{};
            if (!pseudo->native_tty_get_winsize(size))
                return state.complete_reply(empty_bytes(), empty_resources(), ENOTTY) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            append_canonical_winsize(response, size);
        }
        else if (method_id == NA_METHOD_TTY_SET_WINSIZE)
        {
            if (request.size() != sizeof(dev::tty::winsize_t))
                return NA_STATUS_INVALID_MESSAGE;
            dev::tty::winsize_t size{};
            if (!decode_canonical_winsize(request, size))
                return NA_STATUS_INVALID_MESSAGE;
            if (!pseudo->native_tty_set_winsize(size))
                return state.complete_reply(empty_bytes(), empty_resources(), ENOTTY) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
        }
        else if (method_id == NA_METHOD_TTY_FLUSH)
        {
            naos::system::TtyControl::flush_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_flush_request))
                return NA_STATUS_INVALID_MESSAGE;
            const auto result = pseudo->native_tty_flush(static_cast<i32>(decoded.queue));
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        }
        else if (method_id == NA_METHOD_TTY_ATTACH)
        {
            naos::system::TtyControl::attach_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_attach_request))
                return NA_STATUS_INVALID_MESSAGE;
            const auto result = pseudo->native_tty_attach(decoded.controlling != 0);
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        }
        else if (method_id == NA_METHOD_TTY_GET_PGRP)
        {
            naos::system::TtyControl::get_pgrp_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_get_pgrp_request))
                return NA_STATUS_INVALID_MESSAGE;
            u32 group = 0;
            const auto result = pseudo->native_tty_get_pgrp(group);
            if (result != 0)
                return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            naos::system::TtyControl::get_pgrp_response encoded{};
            encoded.group = group;
            if (!encode_message(response, encoded, naos::system::TtyControl::encode_get_pgrp_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        else if (method_id == NA_METHOD_TTY_SET_PGRP)
        {
            naos::system::TtyControl::set_pgrp_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_set_pgrp_request) ||
                decoded.group > 0xffffffffULL)
                return NA_STATUS_INVALID_MESSAGE;
            const auto result = pseudo->native_tty_set_pgrp(static_cast<u32>(decoded.group));
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        }
        else if (method_id == NA_METHOD_TTY_GET_SID)
        {
            naos::system::TtyControl::get_sid_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_get_sid_request))
                return NA_STATUS_INVALID_MESSAGE;
            u32 session = 0;
            const auto result = pseudo->native_tty_get_sid(session);
            if (result != 0)
                return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            naos::system::TtyControl::get_sid_response encoded{};
            encoded.session = session;
            if (!encode_message(response, encoded, naos::system::TtyControl::encode_get_sid_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        else if (method_id == NA_METHOD_TTY_DETACH)
        {
            naos::system::TtyControl::detach_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_detach_request))
                return NA_STATUS_INVALID_MESSAGE;
            const auto result = pseudo->native_tty_detach();
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        }
        else
        {
            naos::system::TtyControl::get_input_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_get_input_request))
                return NA_STATUS_INVALID_MESSAGE;
            u64 count = 0;
            const auto result = pseudo->native_tty_get_input(count);
            if (result != 0)
                return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            naos::system::TtyControl::get_input_response encoded{};
            encoded.count = count;
            if (!encode_message(response, encoded, naos::system::TtyControl::encode_get_input_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_TTY_CONTROL && (method_id == NA_METHOD_PTY_GET_NUMBER || method_id == NA_METHOD_PTY_UNLOCK))
    {
        if (pseudo == nullptr)
            return state.complete_reply(empty_bytes(), empty_resources(), ENOTTY) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        if (method_id == NA_METHOD_PTY_GET_NUMBER)
        {
            naos::system::TtyControl::get_number_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_get_number_request))
                return NA_STATUS_INVALID_MESSAGE;
            u32 number = 0;
            if (!pseudo->native_pty_get_number(number))
                return state.complete_reply(empty_bytes(), empty_resources(), ENOTTY) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            naos::system::TtyControl::get_number_response encoded{};
            encoded.number = number;
            if (!encode_message(response, encoded, naos::system::TtyControl::encode_get_number_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        else
        {
            naos::system::TtyControl::unlock_request decoded{};
            if (!decode_message(request, decoded, naos::system::TtyControl::decode_unlock_request))
                return NA_STATUS_INVALID_MESSAGE;
            if (!pseudo->native_pty_set_locked(decoded.locked != 0))
                return state.complete_reply(empty_bytes(), empty_resources(), ENOTTY) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
        }
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if ((scope == NA_SCOPE_STREAM && method_id == NA_METHOD_STREAM_READ) ||
        (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_PREAD))
    {
        u64 size = 0;
        u64 flags = 0;
        i64 offset = 0;
        if (scope == NA_SCOPE_STREAM)
        {
            naos::system::Stream::read_request decoded{};
            if (!decode_message(request, decoded, naos::system::Stream::decode_read_request))
                return state.complete_failure(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_PROTOCOL_VIOLATION)
                           ? NA_STATUS_OK
                           : NA_STATUS_INVALID_MESSAGE;
            size = decoded.size;
            flags = (decoded.flags & NA_IO_FLAG_NONBLOCK) != 0 ? fs::rw_flags::no_block : 0;
        }
        else
        {
            naos::system::File::pread_request decoded{};
            if (!decode_message(request, decoded, naos::system::File::decode_pread_request))
                return NA_STATUS_INVALID_MESSAGE;
            offset = decoded.offset;
            size = decoded.size;
            flags = (decoded.flags & NA_IO_FLAG_NONBLOCK) != 0 ? fs::rw_flags::no_block : 0;
        }
        if (size > max_kernel_payload)
            return NA_STATUS_INVALID_MESSAGE;
        response.resize(size, byte{});
        const i64 result = scope == NA_SCOPE_STREAM ? file.read(response.data(), size, flags)
                                                    : file.pread(offset, response.data(), size, flags);
        if (result < 0)
        {
            response.clear();
            append_u64(response, static_cast<u64>(-result));
            return state.complete_reply(std::move(response), empty_resources(), result) ? NA_STATUS_OK
                                                                                        : NA_STATUS_PEER_CLOSED;
        }
        const naoidl::bounded_bytes data{reinterpret_cast<const u8 *>(response.data()), static_cast<u32>(result)};
        auto encoded_response = freelibcxx::vector<byte>(allocator);
        if (scope == NA_SCOPE_STREAM)
        {
            naos::system::Stream::read_response encoded{data};
            if (!encode_message(encoded_response, encoded, naos::system::Stream::encode_read_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        else
        {
            naos::system::File::pread_response encoded{data};
            if (!encode_message(encoded_response, encoded, naos::system::File::encode_pread_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        return state.complete_reply(std::move(encoded_response), empty_resources()) ? NA_STATUS_OK
                                                                                    : NA_STATUS_PEER_CLOSED;
    }

    if ((scope == NA_SCOPE_STREAM && method_id == NA_METHOD_STREAM_WRITE) ||
        (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_PWRITE))
    {
        const byte *data = nullptr;
        u64 size = 0;
        i64 offset = 0;
        u64 flags = 0;
        if (scope == NA_SCOPE_STREAM)
        {
            naos::system::Stream::write_request decoded{};
            if (!decode_message(request, decoded, naos::system::Stream::decode_write_request))
                return NA_STATUS_INVALID_MESSAGE;
            size = decoded.size;
            flags = (decoded.flags & NA_IO_FLAG_NONBLOCK) != 0 ? fs::rw_flags::no_block : 0;
            data = reinterpret_cast<const byte *>(decoded.data.data);
        }
        else
        {
            naos::system::File::pwrite_request decoded{};
            if (!decode_message(request, decoded, naos::system::File::decode_pwrite_request))
                return NA_STATUS_INVALID_MESSAGE;
            offset = decoded.offset;
            size = decoded.size;
            flags = (decoded.flags & NA_IO_FLAG_NONBLOCK) != 0 ? fs::rw_flags::no_block : 0;
            data = reinterpret_cast<const byte *>(decoded.data.data);
        }
        const i64 result = scope == NA_SCOPE_STREAM ? file.write(data, size, flags)
                                                    : file.pwrite(offset, data, size, flags);
        if (scope == NA_SCOPE_STREAM)
        {
            naos::system::Stream::write_response encoded{};
            encoded.count = result < 0 ? static_cast<u64>(-result) : static_cast<u64>(result);
            if (!encode_message(response, encoded, naos::system::Stream::encode_write_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        else
        {
            naos::system::File::pwrite_response encoded{};
            encoded.count = result < 0 ? static_cast<u64>(-result) : static_cast<u64>(result);
            if (!encode_message(response, encoded, naos::system::File::encode_pwrite_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        return state.complete_reply(std::move(response), empty_resources(), result < 0 ? result : 0)
                   ? NA_STATUS_OK
                   : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_STREAM && method_id == NA_METHOD_STREAM_POLL)
    {
        naos::system::Stream::poll_request decoded{};
        if (!decode_message(request, decoded, naos::system::Stream::decode_poll_request))
            return NA_STATUS_INVALID_MESSAGE;
        naos::system::Stream::poll_response encoded{};
        encoded.events = pseudo == nullptr ? 0 : pseudo->poll_events();
        if (!encode_message(response, encoded, naos::system::Stream::encode_poll_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_SEEK)
    {
        naos::system::File::seek_request decoded{};
        if (!decode_message(request, decoded, naos::system::File::decode_seek_request))
            return NA_STATUS_INVALID_MESSAGE;
        const auto offset = decoded.offset;
        const auto whence = static_cast<u32>(decoded.whence);
        if (whence == 0)
            file.seek(offset);
        else if (whence == 1)
            file.move(offset);
        else if (whence == 2)
            file.move(file.size() + offset);
        else
            return NA_STATUS_INVALID_ARGUMENT;
        naos::system::File::seek_response encoded{};
        encoded.offset = file.current_offset();
        if (!encode_message(response, encoded, naos::system::File::encode_seek_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_SYNC)
    {
        naos::system::File::sync_request decoded{};
        if (!decode_message(request, decoded, naos::system::File::decode_sync_request))
            return NA_STATUS_INVALID_MESSAGE;
        const auto result = file.native_sync();
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_TRUNCATE)
    {
        naos::system::File::truncate_request decoded{};
        if (!decode_message(request, decoded, naos::system::File::decode_truncate_request))
            return NA_STATUS_INVALID_MESSAGE;
        const auto result = file.native_truncate(decoded.length) ? 0 : EACCES;
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_ALLOCATE)
    {
        naos::system::File::allocate_request decoded{};
        if (!decode_message(request, decoded, naos::system::File::decode_allocate_request))
            return NA_STATUS_INVALID_MESSAGE;
        const auto result = file.native_allocate(decoded.offset, decoded.length) ? 0 : EIO;
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_GET_FLAGS)
    {
        naos::system::File::get_flags_request decoded{};
        if (!decode_message(request, decoded, naos::system::File::decode_get_flags_request))
            return NA_STATUS_INVALID_MESSAGE;
        const auto flags = file.native_get_flags();
        naos::system::File::get_flags_response encoded{};
        encoded.flags = (flags & fs::mode::append) != 0 ? NA_IO_FLAG_APPEND : 0;
        if ((flags & fs::mode::no_block) != 0)
            encoded.flags |= NA_IO_FLAG_NONBLOCK;
        if (!encode_message(response, encoded, naos::system::File::encode_get_flags_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_SET_FLAGS)
    {
        naos::system::File::set_flags_request decoded{};
        if (!decode_message(request, decoded, naos::system::File::decode_set_flags_request))
            return NA_STATUS_INVALID_MESSAGE;
        flag_t flags = 0;
        const auto wire_flags = decoded.flags;
        if ((wire_flags & NA_IO_FLAG_NONBLOCK) != 0)
            flags |= fs::mode::no_block;
        if ((wire_flags & NA_IO_FLAG_APPEND) != 0)
            flags |= fs::mode::append;
        const auto result = file.native_set_flags(flags) ? 0 : EINVAL;
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (scope == NA_SCOPE_FILE && method_id == NA_METHOD_FILE_STAT)
    {
        naos::system::File::stat_request decoded{};
        if (!decode_message(request, decoded, naos::system::File::decode_stat_request))
            return NA_STATUS_INVALID_MESSAGE;
        naos_stat value{};
        if (file.get_entry() == nullptr || !fs::vfs::fill_stat(file.get_entry(), &value))
            return state.complete_reply(empty_bytes(), empty_resources(), EFAILED) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
        append_canonical_stat(response, value);
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_directory_call(invocation_state &state, fs::vfs::native_directory &directory, u64 method_id,
                                   const freelibcxx::vector<byte> &request)
{
    auto response = freelibcxx::vector<byte>(memory::MemoryAllocatorV);
    if (method_id == NA_METHOD_DIRECTORY_SET_CURRENT || method_id == NA_METHOD_DIRECTORY_SET_ROOT)
    {
        bool valid_request = false;
        if (method_id == NA_METHOD_DIRECTORY_SET_CURRENT)
        {
            naos::system::Directory::set_current_request decoded{};
            valid_request = decode_message(request, decoded, naos::system::Directory::decode_set_current_request);
        }
        else
        {
            naos::system::Directory::set_root_request decoded{};
            valid_request = decode_message(request, decoded, naos::system::Directory::decode_set_root_request);
        }
        if (!valid_request || directory.root() == nullptr || directory.current() == nullptr ||
            task::current_process() == nullptr)
            return NA_STATUS_INVALID_MESSAGE;

        auto *process = task::current_process();
        if (method_id == NA_METHOD_DIRECTORY_SET_ROOT)
        {
            process->bootstrap_root_directory =
                handle_t<fs::vfs::native_directory>::make(directory.root(), directory.root());
            process->bootstrap_current_directory =
                handle_t<fs::vfs::native_directory>::make(directory.root(), directory.current());
        }
        else
        {
            const auto root =
                process->bootstrap_root_directory ? process->bootstrap_root_directory->root() : directory.root();
            process->bootstrap_current_directory = handle_t<fs::vfs::native_directory>::make(root, directory.current());
        }
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_DIRECTORY_PATH)
    {
        naos::system::Directory::path_request decoded{};
        if (!decode_message(request, decoded, naos::system::Directory::decode_path_request) ||
            directory.root() == nullptr || directory.current() == nullptr)
            return NA_STATUS_INVALID_MESSAGE;
        char path[4096] = {};
        fs::vfs::pathname(directory.root(), directory.current(), path, sizeof(path));
        if (path[0] == 0)
            return state.complete_reply(empty_bytes(), empty_resources(), EOVERFLOW) ? NA_STATUS_OK
                                                                                     : NA_STATUS_PEER_CLOSED;
        const naoidl::bounded_bytes path_bytes{reinterpret_cast<const u8 *>(path), static_cast<u32>(strlen(path))};
        naos::system::Directory::path_response encoded{path_bytes};
        if (!encode_message(response, encoded, naos::system::Directory::encode_path_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_DIRECTORY_ACCESS)
    {
        naos::system::Directory::access_request decoded{};
        if (!decode_message(request, decoded, naos::system::Directory::decode_access_request) ||
            decoded.path.size == 0 || directory.root() == nullptr || directory.current() == nullptr)
            return NA_STATUS_INVALID_MESSAGE;
        const auto mode = decoded.mode;
        const u64 path_size = decoded.path.size;
        if (decoded.path.data[path_size - 1] != 0)
            return NA_STATUS_INVALID_MESSAGE;
        char *path = reinterpret_cast<char *>(memory::KernelCommonAllocatorV->allocate(path_size, alignof(char)));
        if (path == nullptr)
            return NA_STATUS_RESOURCE_EXHAUSTED;
        memcpy(path, decoded.path.data, path_size);
        flag_t access_mode = 0;
        if ((mode & 1) != 0)
            access_mode |= fs::access_flags::exec;
        if ((mode & 2) != 0)
            access_mode |= fs::access_flags::write;
        if ((mode & 4) != 0)
            access_mode |= fs::access_flags::read;
        if ((mode & 8) != 0)
            access_mode |= fs::access_flags::exist;
        const bool allowed = fs::vfs::access(path, directory.root(), directory.current(), access_mode);
        memory::KernelCommonAllocatorV->deallocate(path);
        return state.complete_reply(empty_bytes(), empty_resources(), allowed ? 0 : EACCES) ? NA_STATUS_OK
                                                                                            : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_DIRECTORY_RENAME || method_id == NA_METHOD_DIRECTORY_LINK ||
        method_id == NA_METHOD_DIRECTORY_SYMLINK)
    {
        const u8 *first_data = nullptr;
        const u8 *second_data = nullptr;
        u64 first_size = 0;
        u64 second_size = 0;
        bool decoded_request = false;
        if (method_id == NA_METHOD_DIRECTORY_RENAME)
        {
            naos::system::Directory::rename_request decoded{};
            decoded_request = decode_message(request, decoded, naos::system::Directory::decode_rename_request);
            if (decoded_request)
            {
                first_data = decoded.first.data;
                second_data = decoded.second.data;
                first_size = decoded.first.size;
                second_size = decoded.second.size;
            }
        }
        else if (method_id == NA_METHOD_DIRECTORY_LINK)
        {
            naos::system::Directory::link_request decoded{};
            decoded_request = decode_message(request, decoded, naos::system::Directory::decode_link_request);
            if (decoded_request)
            {
                first_data = decoded.first.data;
                second_data = decoded.second.data;
                first_size = decoded.first.size;
                second_size = decoded.second.size;
            }
        }
        else
        {
            naos::system::Directory::symlink_request decoded{};
            decoded_request = decode_message(request, decoded, naos::system::Directory::decode_symlink_request);
            if (decoded_request)
            {
                first_data = decoded.first.data;
                second_data = decoded.second.data;
                first_size = decoded.first.size;
                second_size = decoded.second.size;
            }
        }
        if (!decoded_request || first_size == 0 || second_size == 0 || directory.root() == nullptr ||
            directory.current() == nullptr)
            return NA_STATUS_INVALID_MESSAGE;
        char *first = reinterpret_cast<char *>(memory::KernelCommonAllocatorV->allocate(first_size + 1, alignof(char)));
        char *second =
            reinterpret_cast<char *>(memory::KernelCommonAllocatorV->allocate(second_size + 1, alignof(char)));
        if (first == nullptr || second == nullptr)
        {
            if (first != nullptr)
                memory::KernelCommonAllocatorV->deallocate(first);
            if (second != nullptr)
                memory::KernelCommonAllocatorV->deallocate(second);
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        memcpy(first, first_data, first_size);
        memcpy(second, second_data, second_size);
        first[first_size] = 0;
        second[second_size] = 0;
        bool changed = false;
        if (method_id == NA_METHOD_DIRECTORY_RENAME)
            changed = fs::vfs::rename(second, first, directory.root(), directory.current());
        else if (method_id == NA_METHOD_DIRECTORY_LINK)
            changed = fs::vfs::link(second, first, directory.root(), directory.current());
        else
            changed = fs::vfs::symbolink(second, first, directory.root(), directory.current(), 0);
        memory::KernelCommonAllocatorV->deallocate(first);
        memory::KernelCommonAllocatorV->deallocate(second);
        return state.complete_reply(empty_bytes(), empty_resources(), changed ? 0 : ENOENT) ? NA_STATUS_OK
                                                                                            : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_DIRECTORY_READLINK)
    {
        naos::system::Directory::readlink_request decoded{};
        if (!decode_message(request, decoded, naos::system::Directory::decode_readlink_request) ||
            decoded.path.size == 0 || directory.root() == nullptr || directory.current() == nullptr)
            return NA_STATUS_INVALID_MESSAGE;
        const u64 path_size = decoded.path.size;
        char *path = reinterpret_cast<char *>(memory::KernelCommonAllocatorV->allocate(path_size + 1, alignof(char)));
        if (path == nullptr)
            return NA_STATUS_RESOURCE_EXHAUSTED;
        memcpy(path, decoded.path.data, path_size);
        path[path_size] = 0;
        auto *entry = fs::vfs::path_walk(path, directory.root(), directory.current(),
                                         fs::path_walk_flags::not_resolve_symbolic_link);
        const char *target =
            entry != nullptr && entry->get_inode() != nullptr ? entry->get_inode()->symbolink() : nullptr;
        const bool valid = target != nullptr;
        if (valid)
        {
            const naoidl::bounded_bytes target_bytes{reinterpret_cast<const u8 *>(target),
                                                     static_cast<u32>(strlen(target))};
            naos::system::Directory::readlink_response encoded{target_bytes};
            if (!encode_message(response, encoded, naos::system::Directory::encode_readlink_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        memory::KernelCommonAllocatorV->deallocate(path);
        return state.complete_reply(std::move(response), empty_resources(), valid ? 0 : EINVAL) ? NA_STATUS_OK
                                                                                                : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_DIRECTORY_LIST)
    {
        naos::system::Directory::list_request decoded{};
        if (!decode_message(request, decoded, naos::system::Directory::decode_list_request) ||
            directory.current() == nullptr)
            return NA_STATUS_INVALID_MESSAGE;
        const u64 offset = decoded.offset;
        const u64 requested_bytes = decoded.requested_bytes;
        const u64 max_bytes = requested_bytes == 0 ? max_kernel_payload : requested_bytes;
        if (max_bytes > max_kernel_payload)
            return NA_STATUS_INVALID_MESSAGE;
        append_u64(response, offset);
        append_u64(response, 0);
        u64 index = 0;
        u64 next = offset;
        u64 count = 0;
        for (auto *child : directory.current()->list_children())
        {
            if (child == nullptr || child->get_inode() == nullptr)
            {
                index++;
                continue;
            }
            if (index++ < offset)
                continue;
            const char *name = child->get_name();
            if (name == nullptr)
                continue;
            const u64 name_bytes = strlen(name) + 1;
            const u64 record_bytes = sizeof(u64) + sizeof(u32) + sizeof(u32) + name_bytes;
            if (response.size() > max_bytes || record_bytes > max_bytes - response.size())
                break;
            append_u64(response, child->get_inode()->get_index());
            append_u32(response, static_cast<u32>(child->get_inode()->get_type()));
            append_u32(response, static_cast<u32>(name_bytes));
            append_bytes(response, reinterpret_cast<const byte *>(name), name_bytes);
            count++;
            next = index;
        }
        const naoidl::bounded_bytes records{reinterpret_cast<const u8 *>(response.data() + 16),
                                            static_cast<u32>(response.size() - 16)};
        naos::system::Directory::list_response encoded{next, count, records};
        auto encoded_response = freelibcxx::vector<byte>(memory::MemoryAllocatorV);
        if (!encode_message(encoded_response, encoded, naos::system::Directory::encode_list_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(encoded_response), empty_resources()) ? NA_STATUS_OK
                                                                                    : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_DIRECTORY_STAT)
    {
        naos::system::Directory::stat_request decoded{};
        if (!decode_message(request, decoded, naos::system::Directory::decode_stat_request) ||
            directory.current() == nullptr)
            return NA_STATUS_INVALID_MESSAGE;
        naos_stat value{};
        if (!fs::vfs::fill_stat(directory.current(), &value))
            return state.complete_reply(empty_bytes(), empty_resources(), ENOENT) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        append_canonical_stat(response, value);
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_DIRECTORY_CREATE || method_id == NA_METHOD_DIRECTORY_REMOVE ||
        method_id == NA_METHOD_DIRECTORY_OPEN)
    {
        u64 mode = 0;
        u64 flags = 0;
        const u8 *path_bytes = nullptr;
        u64 path_size = 0;
        bool decoded_request = false;
        if (method_id == NA_METHOD_DIRECTORY_CREATE)
        {
            naos::system::Directory::create_request decoded{};
            decoded_request = decode_message(request, decoded, naos::system::Directory::decode_create_request);
            if (decoded_request)
            {
                mode = decoded.mode;
                flags = decoded.flags;
                path_bytes = decoded.path.data;
                path_size = decoded.path.size;
            }
        }
        else if (method_id == NA_METHOD_DIRECTORY_REMOVE)
        {
            naos::system::Directory::remove_request decoded{};
            decoded_request = decode_message(request, decoded, naos::system::Directory::decode_remove_request);
            if (decoded_request)
            {
                mode = decoded.mode;
                flags = decoded.flags;
                path_bytes = decoded.path.data;
                path_size = decoded.path.size;
            }
        }
        else
        {
            naos::system::Directory::open_request decoded{};
            decoded_request = decode_message(request, decoded, naos::system::Directory::decode_open_request);
            if (decoded_request)
            {
                mode = decoded.mode;
                flags = decoded.flags;
                path_bytes = decoded.path.data;
                path_size = decoded.path.size;
            }
        }
        if (!decoded_request || path_size == 0)
            return NA_STATUS_INVALID_MESSAGE;
        if (path_bytes[path_size - 1] != 0)
            return NA_STATUS_INVALID_MESSAGE;
        char *path = reinterpret_cast<char *>(memory::KernelCommonAllocatorV->allocate(path_size, alignof(char)));
        if (path == nullptr)
            return NA_STATUS_RESOURCE_EXHAUSTED;
        memcpy(path, path_bytes, path_size);

        if (method_id == NA_METHOD_DIRECTORY_CREATE)
        {
            const bool created =
                (flags & fs::create_flags::directory) != 0
                    ? fs::vfs::mkdir(path, directory.root(), directory.current(), mode)
                    : fs::vfs::create(path, directory.root(), directory.current(), fs::create_flags::file);
            memory::KernelCommonAllocatorV->deallocate(path);
            if (!created)
                return state.complete_reply(empty_bytes(), empty_resources(), EFAILED) ? NA_STATUS_OK
                                                                                       : NA_STATUS_PEER_CLOSED;
            return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        }
        if (method_id == NA_METHOD_DIRECTORY_REMOVE)
        {
            const bool removed = (flags & fs::create_flags::directory) != 0
                                     ? fs::vfs::rmdir(path, directory.root(), directory.current())
                                     : fs::vfs::unlink(path, directory.root(), directory.current());
            memory::KernelCommonAllocatorV->deallocate(path);
            if (!removed)
                return state.complete_reply(empty_bytes(), empty_resources(), ENOENT) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
            return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        }

        handle_t<fs::vfs::file> file;
        if (method_id == NA_METHOD_DIRECTORY_OPEN && strcmp(path, "/dev/ptmx") == 0)
            file = dev::pty::open_master(mode);
        else if (method_id == NA_METHOD_DIRECTORY_OPEN && has_prefix(path, "/dev/pts/"))
        {
            const char *cursor = path + 9;
            u64 number = 0;
            if (*cursor == 0)
                file.reset();
            else
            {
                bool valid = true;
                for (; *cursor != 0; cursor++)
                {
                    if (*cursor < '0' || *cursor > '9')
                    {
                        valid = false;
                        break;
                    }
                    number = number * 10 + static_cast<u64>(*cursor - '0');
                    if (number > 0xffffffffULL)
                    {
                        valid = false;
                        break;
                    }
                }
                if (valid)
                    file = dev::pty::open_slave(static_cast<u32>(number), mode);
            }
        }
        else
            file = fs::vfs::open(path, directory.root(), directory.current(), mode, flags);
        memory::KernelCommonAllocatorV->deallocate(path);
        if (!file)
            return state.complete_reply(empty_bytes(), empty_resources(), ENOENT) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;

        if (method_id == NA_METHOD_DIRECTORY_OPEN && (flags & fs::path_walk_flags::trunc) != 0 &&
            !file->native_truncate(0))
            return state.complete_reply(empty_bytes(), empty_resources(), EACCES) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;

        capability::metadata metadata;
        khandle object;
        if (file->get_entry() != nullptr && file->get_entry()->get_inode() != nullptr &&
            file->get_entry()->get_inode()->get_type() == fs::inode_type_t::directory)
        {
            metadata = kernel_view_metadata(NA_SCOPE_DIRECTORY, naos::system::Directory::protocol_uuid,
                                            NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                            NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT);
            const auto opened_root =
                (flags & NA_DIRECTORY_OPEN_FLAG_CHROOT) != 0 ? file->get_entry() : directory.root();
            auto opened_directory = handle_t<fs::vfs::native_directory>::make(opened_root, file->get_entry());
            object = std::move(opened_directory);
        }
        else
        {
            metadata = kernel_view_metadata(NA_SCOPE_FILE, naos::system::File::protocol_uuid,
                                            NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                            NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT);
            object = khandle(file);
        }
        capability::transfer_record_list resources(memory::KernelCommonAllocatorV);
        resources.push_back(make_response_resource(std::move(object), metadata));
        naos::system::Directory::open_response encoded{};
        encoded.object.value = 0;
        if (!encode_message(response, encoded, naos::system::Directory::encode_open_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), std::move(resources)) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_service_directory_call(invocation_state &state, service::directory &directory, u64 method_id,
                                            const freelibcxx::vector<byte> &request,
                                            capability::transfer_record_list &resources)
{
    if (method_id == NA_METHOD_SERVICE_DIRECTORY_REGISTER)
    {
        naos::system::ServiceDirectory::register_request decoded{};
        if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_register_request) ||
            !naos::system::ServiceDirectory::validate_register_request_resources(decoded, resources.size()) ||
            decoded.service.value >= resources.size())
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

        const auto result = directory.register_service(reinterpret_cast<const char *>(decoded.uri.data),
                                                       decoded.uri.size, resources[decoded.service.value]);
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_RESOLVE)
    {
        naos::system::ServiceDirectory::resolve_request decoded{};
        if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_resolve_request))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

        capability::transferred_resource resource;
        const auto result = directory.resolve_service(reinterpret_cast<const char *>(decoded.uri.data),
                                                       decoded.uri.size, resource);
        if (result != 0)
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

        naos::system::ServiceDirectory::resolve_response response{};
        response.service.value = 0;
        freelibcxx::vector<byte> encoded_response(memory::MemoryAllocatorV);
        if (!encode_message(encoded_response, response, naos::system::ServiceDirectory::encode_resolve_response))
            return state.complete_reply(empty_bytes(), empty_resources(), ENOMEM) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        capability::transfer_record_list response_resources(memory::KernelCommonAllocatorV);
        response_resources.push_back(capability::transfer_record(NA_HANDLE_INVALID, true, std::move(resource)));
        return state.complete_reply(std::move(encoded_response), std::move(response_resources)) ? NA_STATUS_OK
                                                                                                  : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_UNREGISTER)
    {
        naos::system::ServiceDirectory::unregister_request decoded{};
        if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_unregister_request))
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        const auto result = directory.unregister_service(reinterpret_cast<const char *>(decoded.uri.data),
                                                         decoded.uri.size);
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_SERVICE_DIRECTORY_LIST)
    {
        naos::system::ServiceDirectory::list_request decoded{};
        if (!decode_message(request, decoded, naos::system::ServiceDirectory::decode_list_request) ||
            decoded.requested_bytes > max_kernel_payload)
            return state.complete_reply(empty_bytes(), empty_resources(), EINVAL) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

        freelibcxx::vector<byte> records(memory::MemoryAllocatorV);
        u64 next = 0;
        u64 count = 0;
        const auto result = directory.list_services(decoded.offset, decoded.requested_bytes, records, next, count);
        if (result != 0)
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;

        naos::system::ServiceDirectory::list_response response{};
        response.next = next;
        response.count = count;
        response.records = {reinterpret_cast<const u8 *>(records.data()), static_cast<u32>(records.size())};
        freelibcxx::vector<byte> encoded_response(memory::MemoryAllocatorV);
        if (!encode_message(encoded_response, response, naos::system::ServiceDirectory::encode_list_response))
            return state.complete_reply(empty_bytes(), empty_resources(), ENOMEM) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        return state.complete_reply(std::move(encoded_response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_process_call(invocation_state &state, task::process_object &process, u64 method_id,
                                 const freelibcxx::vector<byte> &request)
{
    auto response = freelibcxx::vector<byte>(memory::MemoryAllocatorV);
    auto *target = process.process();
    if (target == nullptr)
        return NA_STATUS_OBJECT_REVOKED;

    if (method_id == NA_METHOD_PROCESS_WAIT)
    {
        naos::system::Process::wait_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_wait_request))
            return NA_STATUS_INVALID_MESSAGE;
        i64 status = 0;
        process_id waited_pid = 0;
        const auto result = static_cast<i64>(task::wait_process_handle(
            task::current_process(), target, static_cast<flag_t>(decoded.flags), status, waited_pid));
        if (result != 0)
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        naos::system::Process::wait_response encoded{};
        encoded.status = status;
        encoded.pid = waited_pid;
        if (!encode_message(response, encoded, naos::system::Process::encode_wait_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_WAIT_CHILDREN)
    {
        if (target != task::current_process())
            return NA_STATUS_ACCESS_DENIED;
        naos::system::Process::wait_children_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_wait_children_request))
            return NA_STATUS_INVALID_MESSAGE;
        i64 status = 0;
        process_id waited_pid = 0;
        const auto result = task::wait_process_children(task::current_process(), decoded.pid,
                                                        static_cast<flag_t>(decoded.flags), status, waited_pid);
        if (result != 0)
            return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        naos::system::Process::wait_children_response encoded{};
        encoded.status = status;
        encoded.pid = waited_pid;
        if (!encode_message(response, encoded, naos::system::Process::encode_wait_children_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_GET_INFO)
    {
        naos::system::Process::get_info_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_get_info_request))
            return NA_STATUS_INVALID_MESSAGE;
        naos::system::Process::get_info_response encoded{};
        encoded.pid = target->pid;
        encoded.parent_pid = target->parent_pid;
        encoded.attributes = target->attributes.load();
        encoded.return_value = target->ret_val;
        if (!encode_message(response, encoded, naos::system::Process::encode_get_info_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_GET_JOB_CONTROL_INFO)
    {
        naos::system::Process::get_job_control_info_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_get_job_control_info_request))
            return NA_STATUS_INVALID_MESSAGE;
        task::job_control_info info{};
        if (!task::get_job_control_info(target, info))
            return NA_STATUS_OBJECT_REVOKED;
        naos::system::Process::get_job_control_info_response encoded{};
        encoded.session = static_cast<i64>(info.session);
        encoded.process_group = static_cast<i64>(info.process_group);
        encoded.foreground_process_group = static_cast<i64>(info.foreground_process_group);
        encoded.has_controlling_tty = info.has_controlling_tty ? 1 : 0;
        if (!encode_message(response, encoded, naos::system::Process::encode_get_job_control_info_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_SET_SESSION)
    {
        if (target != task::current_process())
            return NA_STATUS_ACCESS_DENIED;
        naos::system::Process::set_session_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_set_session_request))
            return NA_STATUS_INVALID_MESSAGE;
        const auto session = task::setsid(target);
        if (session < 0)
            return state.complete_reply(empty_bytes(), empty_resources(), session) ? NA_STATUS_OK
                                                                                   : NA_STATUS_PEER_CLOSED;
        naos::system::Process::set_session_response encoded{};
        encoded.session = session;
        if (!encode_message(response, encoded, naos::system::Process::encode_set_session_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_GET_PROCESS_GROUP || method_id == NA_METHOD_PROCESS_GET_SESSION)
    {
        task::job_control_info info{};
        if (!task::get_job_control_info(target, info))
            return NA_STATUS_OBJECT_REVOKED;
        if (method_id == NA_METHOD_PROCESS_GET_PROCESS_GROUP)
        {
            naos::system::Process::get_process_group_request decoded{};
            if (!decode_message(request, decoded, naos::system::Process::decode_get_process_group_request))
                return NA_STATUS_INVALID_MESSAGE;
            naos::system::Process::get_process_group_response encoded{};
            encoded.process_group = static_cast<i64>(info.process_group);
            if (!encode_message(response, encoded, naos::system::Process::encode_get_process_group_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        else
        {
            naos::system::Process::get_session_request decoded{};
            if (!decode_message(request, decoded, naos::system::Process::decode_get_session_request))
                return NA_STATUS_INVALID_MESSAGE;
            naos::system::Process::get_session_response encoded{};
            encoded.session = static_cast<i64>(info.session);
            if (!encode_message(response, encoded, naos::system::Process::encode_get_session_response))
                return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_PROCESS_SET_PROCESS_GROUP)
    {
        naos::system::Process::set_process_group_request decoded{};
        if (!decode_message(request, decoded, naos::system::Process::decode_set_process_group_request))
            return NA_STATUS_INVALID_MESSAGE;
        if (decoded.process_group < 0)
            return NA_STATUS_INVALID_ARGUMENT;
        const auto result = task::setpgid(task::current_process(), target->pid,
                                          static_cast<group_id>(decoded.process_group));
        return state.complete_reply(empty_bytes(), empty_resources(), result) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_memory_object_call(invocation_state &state, data_plane::memory_object &memory_object, u64 method_id,
                                       const freelibcxx::vector<byte> &request)
{
    auto response = freelibcxx::vector<byte>(memory::MemoryAllocatorV);
    if (method_id == NA_METHOD_MEMORY_OBJECT_GET_INFO)
    {
        naos::system::MemoryObject::get_info_request decoded{};
        if (!decode_message(request, decoded, naos::system::MemoryObject::decode_get_info_request))
            return NA_STATUS_INVALID_MESSAGE;
        naos::system::MemoryObject::get_info_response encoded{};
        encoded.flags = memory_object.flags();
        encoded.size = memory_object.size();
        encoded.max_size = NA_MEMORY_OBJECT_MAX_BYTES;
        if (!encode_message(response, encoded, naos::system::MemoryObject::encode_get_info_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_MEMORY_OBJECT_READ)
    {
        naos::system::MemoryObject::read_request decoded{};
        if (!decode_message(request, decoded, naos::system::MemoryObject::decode_read_request) ||
            decoded.size > max_kernel_payload)
            return NA_STATUS_INVALID_MESSAGE;

        freelibcxx::vector<byte> data(memory::MemoryAllocatorV);
        data.resize(decoded.size, byte{});
        if (decoded.size != 0 && data.data() == nullptr)
            return NA_STATUS_RESOURCE_EXHAUSTED;

        u64 actual = 0;
        const auto status = memory_object.read(decoded.offset, data.data(), decoded.size, actual);
        if (status != NA_STATUS_OK)
            return state.complete_reply(empty_bytes(), empty_resources(), status) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;

        naos::system::MemoryObject::read_response encoded{};
        encoded.data = {reinterpret_cast<const u8 *>(data.data()), static_cast<u32>(actual)};
        if (!encode_message(response, encoded, naos::system::MemoryObject::encode_read_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_MEMORY_OBJECT_WRITE)
    {
        naos::system::MemoryObject::write_request decoded{};
        if (!decode_message(request, decoded, naos::system::MemoryObject::decode_write_request))
            return NA_STATUS_INVALID_MESSAGE;
        if (decoded.size != decoded.data.size)
            return NA_STATUS_INVALID_MESSAGE;

        u64 actual = 0;
        const auto status = memory_object.write(decoded.offset, reinterpret_cast<const byte *>(decoded.data.data),
                                                decoded.data.size, actual);
        if (status != NA_STATUS_OK)
            return state.complete_reply(empty_bytes(), empty_resources(), status) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;

        naos::system::MemoryObject::write_response encoded{};
        encoded.count = actual;
        if (!encode_message(response, encoded, naos::system::MemoryObject::encode_write_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t publish_shared_ring_call(invocation_state &state, data_plane::shared_ring &ring, u64 method_id,
                                     const freelibcxx::vector<byte> &request)
{
    auto response = freelibcxx::vector<byte>(memory::MemoryAllocatorV);
    if (method_id == NA_METHOD_SHARED_RING_GET_INFO)
    {
        naos::system::SharedRing::get_info_request decoded{};
        if (!decode_message(request, decoded, naos::system::SharedRing::decode_get_info_request))
            return NA_STATUS_INVALID_MESSAGE;
        naos::system::SharedRing::get_info_response encoded{};
        encoded.slots = ring.slots();
        encoded.slot_bytes = ring.slot_bytes();
        encoded.queued = ring.queued();
        if (!encode_message(response, encoded, naos::system::SharedRing::encode_get_info_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_SHARED_RING_PUSH)
    {
        naos::system::SharedRing::push_request decoded{};
        if (!decode_message(request, decoded, naos::system::SharedRing::decode_push_request) ||
            decoded.size != decoded.data.size)
            return NA_STATUS_INVALID_MESSAGE;

        freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
        bytes.resize(decoded.data.size, byte{});
        if (decoded.data.size != 0 && bytes.data() == nullptr)
            return NA_STATUS_RESOURCE_EXHAUSTED;
        for (u32 i = 0; i < decoded.data.size; i++)
            bytes[i] = static_cast<byte>(decoded.data.data[i]);

        const auto status = ring.push(std::move(bytes));
        if (status != NA_STATUS_OK)
            return state.complete_reply(empty_bytes(), empty_resources(), status) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;
        naos::system::SharedRing::push_response encoded{};
        if (!encode_message(response, encoded, naos::system::SharedRing::encode_push_response))
            return NA_STATUS_RESOURCE_EXHAUSTED;
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    if (method_id == NA_METHOD_SHARED_RING_POP)
    {
        naos::system::SharedRing::pop_request decoded{};
        if (!decode_message(request, decoded, naos::system::SharedRing::decode_pop_request))
            return NA_STATUS_INVALID_MESSAGE;

        freelibcxx::vector<byte> snapshot(memory::MemoryAllocatorV);
        auto status = ring.claim_pop(snapshot);
        if (status != NA_STATUS_OK)
            return state.complete_reply(empty_bytes(), empty_resources(), status) ? NA_STATUS_OK
                                                                                  : NA_STATUS_PEER_CLOSED;

        naos::system::SharedRing::pop_response encoded{};
        encoded.required_bytes = snapshot.size();
        if (decoded.capacity < snapshot.size())
        {
            ring.cancel_pop();
        }
        else
        {
            encoded.data = {reinterpret_cast<const u8 *>(snapshot.data()), static_cast<u32>(snapshot.size())};
        }

        if (!encode_message(response, encoded, naos::system::SharedRing::encode_pop_response))
        {
            ring.cancel_pop();
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }

        if (decoded.capacity >= snapshot.size())
        {
            status = ring.commit_pop();
            if (status != NA_STATUS_OK)
                return state.complete_reply(empty_bytes(), empty_resources(), status) ? NA_STATUS_OK
                                                                                      : NA_STATUS_PEER_CLOSED;
        }
        return state.complete_reply(std::move(response), empty_resources()) ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
    }

    return NA_STATUS_NOT_SUPPORTED;
}

na_status_t dispatch_kernel_view(capability::entry &target, invocation_state &state, u64 method_id,
                                 const freelibcxx::vector<byte> &request,
                                 capability::transfer_record_list &resources)
{
    state.mark_dispatched();
    if (auto *file = target.object->get<fs::vfs::file>())
    {
        if (target.meta.scope != NA_SCOPE_FILE && target.meta.scope != NA_SCOPE_STREAM &&
            target.meta.scope != NA_SCOPE_TTY_CONTROL)
            return NA_STATUS_WRONG_SCOPE;
        return publish_file_call(state, *file, target.meta.scope, method_id, request);
    }
    if (auto *directory = target.object->get<fs::vfs::native_directory>())
    {
        if (target.meta.scope != NA_SCOPE_DIRECTORY)
            return NA_STATUS_WRONG_SCOPE;
        return publish_directory_call(state, *directory, method_id, request);
    }
    if (auto *process = target.object->get<task::process_object>())
    {
        if (target.meta.scope != NA_SCOPE_PROCESS)
            return NA_STATUS_WRONG_SCOPE;
        if (method_id == NA_METHOD_PROCESS_WAIT &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_WAIT)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_PROCESS_WAIT_CHILDREN &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_WAIT)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_PROCESS_GET_INFO &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_INSPECT)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if ((method_id == NA_METHOD_PROCESS_GET_JOB_CONTROL_INFO || method_id == NA_METHOD_PROCESS_GET_PROCESS_GROUP ||
             method_id == NA_METHOD_PROCESS_GET_SESSION) &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_INSPECT)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if ((method_id == NA_METHOD_PROCESS_SET_SESSION || method_id == NA_METHOD_PROCESS_SET_PROCESS_GROUP) &&
            (target.meta.protocol_rights & static_cast<u64>(NA_PROCESS_RIGHT_JOB_CONTROL)) == 0)
            return NA_STATUS_ACCESS_DENIED;
        return publish_process_call(state, *process, method_id, request);
    }
    if (auto *memory_object = target.object->get<data_plane::memory_object>())
    {
        if (target.meta.scope != NA_SCOPE_MEMORY_OBJECT)
            return NA_STATUS_WRONG_SCOPE;
        if (method_id == NA_METHOD_MEMORY_OBJECT_GET_INFO &&
            ((target.meta.meta_rights & NA_RIGHT_INSPECT) == 0 ||
             (target.meta.protocol_rights & NA_MEMORY_RIGHT_INFO) == 0))
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_MEMORY_OBJECT_READ && (target.meta.protocol_rights & NA_MEMORY_RIGHT_READ) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_MEMORY_OBJECT_WRITE && (target.meta.protocol_rights & NA_MEMORY_RIGHT_WRITE) == 0)
            return NA_STATUS_ACCESS_DENIED;
        return publish_memory_object_call(state, *memory_object, method_id, request);
    }
    if (auto *ring = target.object->get<data_plane::shared_ring>())
    {
        if (target.meta.scope != NA_SCOPE_SHARED_RING)
            return NA_STATUS_WRONG_SCOPE;
        if (method_id == NA_METHOD_SHARED_RING_GET_INFO && ((target.meta.meta_rights & NA_RIGHT_INSPECT) == 0 ||
                                                            (target.meta.protocol_rights & NA_RING_RIGHT_INFO) == 0))
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_SHARED_RING_PUSH && (target.meta.protocol_rights & NA_RING_RIGHT_PUSH) == 0)
            return NA_STATUS_ACCESS_DENIED;
        if (method_id == NA_METHOD_SHARED_RING_POP && (target.meta.protocol_rights & NA_RING_RIGHT_POP) == 0)
            return NA_STATUS_ACCESS_DENIED;
        return publish_shared_ring_call(state, *ring, method_id, request);
    }
    if (auto *directory = target.object->get<service::directory>())
    {
        if (target.meta.scope != NA_SCOPE_SERVICE_DIRECTORY)
            return NA_STATUS_WRONG_SCOPE;
        return publish_service_directory_call(state, *directory, method_id, request, resources);
    }
    return NA_STATUS_WRONG_BINDING;
}

bool endpoint_is_client(capability::entry &entry, protocol_endpoint *&endpoint)
{
    if (entry.meta.binding != NA_BINDING_CLIENT_END || !entry.object)
        return false;
    endpoint = entry.object->get_unsafe<protocol_endpoint>();
    return endpoint != nullptr && endpoint->role() == endpoint_role::client;
}

bool endpoint_is_server(capability::entry &entry, protocol_endpoint *&endpoint)
{
    if (entry.meta.binding != NA_BINDING_SERVER_END || !entry.object)
        return false;
    endpoint = entry.object->get_unsafe<protocol_endpoint>();
    return endpoint != nullptr && endpoint->role() == endpoint_role::server;
}

bool descriptor_allows_method(const na_protocol_descriptor_t &descriptor, u64 method_id)
{
    if (method_id == 0 || method_id > descriptor.method_count || method_id > NA_PROTOCOL_MAX_METHOD_ID)
        return false;
    const u64 word = (method_id - 1) / 64;
    const u64 bit = (method_id - 1) % 64;
    return (descriptor.method_bitmap[word] & (1ULL << bit)) != 0;
}

bool descriptor_allows_oneway(const na_protocol_descriptor_t &descriptor, u64 method_id)
{
    if (!descriptor_allows_method(descriptor, method_id))
        return false;
    const u64 word = (method_id - 1) / 64;
    const u64 bit = (method_id - 1) % 64;
    return (descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0;
}

na_status_t validate_descriptor(const na_protocol_descriptor_t &descriptor)
{
    bool uuid_is_zero = true;
    for (auto value : descriptor.uuid.bytes)
        uuid_is_zero = uuid_is_zero && value == 0;
    if (!valid_frame_size(descriptor.struct_size, sizeof(descriptor)) ||
        (descriptor.flags & ~NA_PROTOCOL_FLAG_ALLOW_ONEWAY) != 0 || descriptor.reserved0 != 0 ||
        descriptor.reserved1 != 0 || uuid_is_zero || descriptor.scope == NA_SCOPE_NONE ||
        descriptor.method_count == 0 || descriptor.method_count > NA_PROTOCOL_MAX_METHOD_ID ||
        descriptor.max_request_bytes > max_kernel_payload || descriptor.max_response_bytes > max_kernel_payload ||
        descriptor.max_resources > NA_CHANNEL_MAX_RESOURCES ||
        (descriptor.protocol_rights & NA_PROTOCOL_RIGHT_INVOKE) == 0 ||
        (descriptor.method_bitmap[(descriptor.method_count - 1) / 64] &
         (1ULL << ((descriptor.method_count - 1) % 64))) == 0)
        return NA_STATUS_INVALID_ARGUMENT;
    for (u64 method_id = descriptor.method_count + 1; method_id <= NA_PROTOCOL_MAX_METHOD_ID; method_id++)
    {
        const u64 word = (method_id - 1) / 64;
        const u64 bit = (method_id - 1) % 64;
        if ((descriptor.method_bitmap[word] & (1ULL << bit)) != 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if ((descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0)
            return NA_STATUS_INVALID_ARGUMENT;
    }
    for (u64 method_id = 1; method_id <= descriptor.method_count; method_id++)
    {
        const u64 word = (method_id - 1) / 64;
        const u64 bit = (method_id - 1) % 64;
        if ((descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0 &&
            (descriptor.method_bitmap[word] & (1ULL << bit)) == 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if ((descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0 &&
            (descriptor.flags & NA_PROTOCOL_FLAG_ALLOW_ONEWAY) == 0)
            return NA_STATUS_INVALID_ARGUMENT;
    }
    return NA_STATUS_OK;
}

} // namespace

protocol_descriptor::protocol_descriptor(const na_protocol_descriptor_t &descriptor)
    : kobject(type_e::protocol_descriptor)
    , descriptor_(descriptor)
{
}

bool protocol_descriptor::matches(const na_uuid_t &uuid) const
{
    return memcmp(descriptor_.uuid.bytes, uuid.bytes, sizeof(uuid.bytes)) == 0;
}

protocol_endpoint::protocol_endpoint(handle_t<protocol_state> state, endpoint_role role)
    : kobject(type_of(role))
    , state_(std::move(state))
    , role_(role)
{
}

protocol_endpoint::~protocol_endpoint()
{
    if (role_ == endpoint_role::server && state_)
        state_->close_server_queue();
}

void protocol_endpoint::on_capability_acquire(capability::location where)
{
    if (state_)
        state_->endpoint_acquired(role_, where);
}

void protocol_endpoint::on_capability_release(capability::location where)
{
    if (state_)
        state_->endpoint_released(role_, where);
}

void protocol_endpoint::on_capability_handoff(capability::location from, capability::location to)
{
    (void)to;
    if (state_)
        state_->endpoint_released(role_, from);
}

na_signal_t protocol_endpoint::capability_signals() const { return state_ ? state_->signals(role_) : 0; }

u64 protocol_endpoint::capability_state() const { return static_cast<u64>(role_); }

void protocol_endpoint::begin_operation()
{
    if (state_)
        state_->begin_operation();
}

void protocol_endpoint::end_operation()
{
    if (state_)
        state_->end_operation();
}

invocation_state::invocation_state(u64 method_id, u64 operation_budget, u64 max_response_bytes,
                                   u64 max_response_resources)
    : phase_(invocation_phase::queued)
    , method_id_(method_id)
    , operation_deadline_(calculate_deadline(operation_budget))
    , max_response_bytes_(max_response_bytes == 0 ? max_kernel_payload : max_response_bytes)
    , max_response_resources_(max_response_resources == 0 ? NA_CHANNEL_MAX_RESOURCES : max_response_resources)
    , result_claimed_(false)
    , responder_alive_(true)
    , client_closed_(false)
    , cancellation_requested_(false)
    , result_budget_reserved_(false)
    , execution_outcome_(NA_EXECUTION_NONE)
    , outcome_reason_(NA_OUTCOME_REASON_NONE)
    , protocol_error_(0)
    , queue_owner_(nullptr)
    , response_bytes_(memory::MemoryAllocatorV)
    , response_resources_(memory::KernelCommonAllocatorV)
{
}

invocation_state::~invocation_state()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    release_result_budget_locked();
}

void invocation_state::release_result_budget_locked()
{
    if (!result_budget_reserved_)
        return;
    release_protocol_global(max_response_bytes_, max_response_resources_);
    result_budget_reserved_ = false;
}

bool invocation_state::reserve_result_budget()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (result_budget_reserved_)
        return true;
    if (!reserve_protocol_global(max_response_bytes_, max_response_resources_))
        return false;
    result_budget_reserved_ = true;
    return true;
}

na_signal_t invocation_state::signals() const
{
    auto &lock = const_cast<lock::spinlock_t &>(lock_);
    uctx::RawSpinLockUninterruptibleContext guard(lock);
    na_signal_t result = 0;
    if (phase_ == invocation_phase::ready)
        result |= NA_SIGNAL_COMPLETED;
    if (phase_ == invocation_phase::consumed)
        result |= NA_SIGNAL_COMPLETED;
    if (client_closed_ && phase_ != invocation_phase::consumed)
        result |= NA_SIGNAL_PEER_CLOSED;
    if (cancellation_requested_ && phase_ != invocation_phase::consumed)
        result |= NA_SIGNAL_CANCEL_REQUESTED;
    return result;
}

bool invocation_state::publish_locked(na_execution_outcome_t outcome, na_outcome_reason_t reason,
                                      freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources,
                                      i64 protocol_error)
{
    if (phase_ == invocation_phase::ready || phase_ == invocation_phase::consumed || client_closed_)
        return false;
    phase_ = invocation_phase::ready;
    execution_outcome_ = outcome;
    outcome_reason_ = reason;
    // Kernel adapters use POSIX errno values internally. The object-call ABI
    // carries a signed protocol error, so publish a single canonical form.
    protocol_error_ = protocol_error > 0 ? -protocol_error : protocol_error;
    if (!client_closed_)
    {
        response_bytes_ = std::move(bytes);
        response_resources_ = std::move(resources);
    }
    wake_invocation_waiters();
    wait_queue_.do_wake_up();
    return true;
}

bool invocation_state::begin_receive()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (phase_ != invocation_phase::queued)
        return false;
    phase_ = invocation_phase::receiving;
    return true;
}

void invocation_state::rollback_receive()
{
    bool cancelled = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (phase_ != invocation_phase::receiving)
            return;
        phase_ = invocation_phase::queued;
        cancelled = cancellation_requested_;
    }
    if (cancelled)
        complete_not_delivered(NA_OUTCOME_REASON_CANCEL_REQUESTED);
}

bool invocation_state::finish_dispatch()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (phase_ != invocation_phase::receiving)
        return false;
    phase_ = invocation_phase::dispatched;
    return true;
}

void invocation_state::set_queue_owner(const handle_t<protocol_state> &owner)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (phase_ == invocation_phase::queued)
        queue_owner_ = owner;
}

void invocation_state::clear_queue_owner()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    queue_owner_.reset();
}

bool invocation_state::cancellation_requested() const
{
    auto &lock = const_cast<lock::spinlock_t &>(lock_);
    uctx::RawSpinLockUninterruptibleContext guard(lock);
    return cancellation_requested_;
}

void invocation_state::mark_dispatched()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (phase_ == invocation_phase::queued || phase_ == invocation_phase::receiving)
        phase_ = invocation_phase::dispatched;
}

bool invocation_state::cancel(protocol_state *queue_owner)
{
    invocation_request *removed = nullptr;
    bool cancelled = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (phase_ == invocation_phase::queued)
        {
            cancellation_requested_ = true;
            auto *owner = queue_owner != nullptr ? queue_owner : queue_owner_.operator&();
            if (owner != nullptr)
            {
                removed = owner->remove_queued(this);
                if (removed == nullptr)
                {
                    // A receiver may have claimed the request between the
                    // state check and queue removal.  Leave it queued for
                    // that receive transaction and expose cancellation as a
                    // best-effort signal.
                    wake_invocation_waiters();
                    return true;
                }
            }
            cancelled = publish_locked(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_CANCEL_REQUESTED, empty_bytes(),
                                       empty_resources(), 0);
        }
        else if (phase_ == invocation_phase::receiving || phase_ == invocation_phase::dispatched)
        {
            cancellation_requested_ = true;
            wake_invocation_waiters();
            cancelled = true;
        }
    }
    if (removed != nullptr)
        removed->state->clear_queue_owner();
    if (removed != nullptr)
        memory::Delete<>(memory::KernelCommonAllocatorV, removed);
    return cancelled;
}

na_status_t invocation_state::arm_deadline(const handle_t<invocation_state> &self)
{
    if (operation_deadline_ == no_deadline)
        return NA_STATUS_OK;
    auto *watch = memory::New<deadline_watch>(memory::KernelCommonAllocatorV, self);
    if (watch == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    if (timer::schedule_at(operation_deadline_, timer::timer_handler::bind<&deadline_watch::invoke>(*watch)) ==
        timer::invalid_watcher_id)
    {
        memory::Delete<>(memory::KernelCommonAllocatorV, watch);
        expire_deadline();
    }
    return NA_STATUS_OK;
}

void invocation_state::expire_deadline()
{
    if (operation_deadline_ == no_deadline || timer::get_high_resolution_time() < operation_deadline_)
        return;

    handle_t<protocol_state> owner;
    deadline_action action = deadline_action::none;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        const auto phase = static_cast<deadline_phase>(phase_);
        action = deadline_action_for(phase, true);
        if (action == deadline_action::remove_queued)
            owner = queue_owner_;
    }

    if (action == deadline_action::remove_queued && owner)
    {
        auto *request = owner->remove_queued(this);
        if (request != nullptr)
        {
            clear_queue_owner();
            {
                uctx::RawSpinLockUninterruptibleContext guard(lock_);
                if (phase_ == invocation_phase::queued)
                    publish_locked(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_OPERATION_DEADLINE, empty_bytes(),
                                   empty_resources(), 0);
            }
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            return;
        }
    }

    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (phase_ == invocation_phase::queued)
    {
        publish_locked(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_OPERATION_DEADLINE, empty_bytes(),
                       empty_resources(), 0);
    }
    else if (phase_ == invocation_phase::receiving || phase_ == invocation_phase::dispatched)
    {
        responder_alive_ = false;
        publish_locked(NA_EXECUTION_OUTCOME_UNKNOWN, NA_OUTCOME_REASON_OPERATION_DEADLINE, empty_bytes(),
                       empty_resources(), 0);
    }
}

void invocation_state::close_client()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    client_closed_ = true;
    response_bytes_.clear();
    response_resources_.clear();
    release_result_budget_locked();
}

void invocation_state::abandon_responder()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (!responder_alive_)
        return;
    responder_alive_ = false;
    if (phase_ == invocation_phase::queued)
        publish_locked(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_RESPONDER_ABANDONED, empty_bytes(),
                       empty_resources(), 0);
    else if (phase_ == invocation_phase::dispatched)
        publish_locked(NA_EXECUTION_OUTCOME_UNKNOWN, NA_OUTCOME_REASON_RESPONDER_ABANDONED, empty_bytes(),
                       empty_resources(), 0);
}

bool invocation_state::consume_responder()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (!responder_alive_)
        return false;
    responder_alive_ = false;
    return true;
}

bool invocation_state::complete_reply(freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources,
                                      i64 protocol_error)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    return publish_locked(NA_EXECUTION_NONE, NA_OUTCOME_REASON_NONE, std::move(bytes), std::move(resources),
                          protocol_error);
}

bool invocation_state::complete_reply(freelibcxx::vector<byte> &bytes, capability::transfer_record_list &resources,
                                      i64 protocol_error)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (phase_ == invocation_phase::ready || phase_ == invocation_phase::consumed)
        return false;
    return publish_locked(NA_EXECUTION_NONE, NA_OUTCOME_REASON_NONE, std::move(bytes), std::move(resources),
                          protocol_error);
}

bool invocation_state::complete_failure(na_execution_outcome_t outcome, na_outcome_reason_t reason)
{
    if (!valid_failure(outcome, reason))
        return false;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    return publish_locked(outcome, reason, empty_bytes(), empty_resources(), 0);
}

bool invocation_state::complete_not_delivered(na_outcome_reason_t reason)
{
    return complete_failure(NA_EXECUTION_NOT_DELIVERED, reason);
}

bool invocation_state::response_within_limits(u64 bytes, u64 resources) const
{
    auto &lock = const_cast<lock::spinlock_t &>(lock_);
    uctx::RawSpinLockUninterruptibleContext guard(lock);
    return bytes <= max_response_bytes_ && resources <= max_response_resources_;
}

bool invocation_state::deadline_expired(bool dispatched_unknown)
{
    (void)dispatched_unknown;
    if (operation_deadline_ == no_deadline || timer::get_high_resolution_time() < operation_deadline_)
        return false;
    expire_deadline();
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    return phase_ == invocation_phase::ready || phase_ == invocation_phase::consumed;
}

na_status_t invocation_state::claim_result(na_result_frame_t &frame, freelibcxx::vector<byte> &bytes,
                                           capability::transfer_record_list &resources)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (phase_ == invocation_phase::consumed)
        return NA_STATUS_ALREADY_CONSUMED;
    if (phase_ != invocation_phase::ready)
    {
        if (operation_deadline_ != no_deadline && timer::get_high_resolution_time() >= operation_deadline_)
        {
            const auto outcome =
                phase_ == invocation_phase::dispatched ? NA_EXECUTION_OUTCOME_UNKNOWN : NA_EXECUTION_NOT_DELIVERED;
            publish_locked(outcome, NA_OUTCOME_REASON_OPERATION_DEADLINE, empty_bytes(), empty_resources(), 0);
        }
        if (phase_ != invocation_phase::ready)
            return NA_STATUS_WOULD_BLOCK;
    }
    if (result_claimed_)
        return NA_STATUS_WOULD_BLOCK;

    frame.method_id = method_id_;
    frame.actual_bytes = response_bytes_.size();
    frame.actual_resources = response_resources_.size();
    frame.required_bytes = response_bytes_.size();
    frame.required_resources = response_resources_.size();
    frame.execution_outcome = execution_outcome_;
    frame.outcome_reason = outcome_reason_;
    frame.protocol_error = protocol_error_;
    if (frame.byte_capacity < response_bytes_.size() || frame.resource_capacity < response_resources_.size())
        return NA_STATUS_BUFFER_TOO_SMALL;
    result_claimed_ = true;
    bytes = std::move(response_bytes_);
    resources = std::move(response_resources_);
    frame.actual_bytes = bytes.size();
    frame.actual_resources = resources.size();
    frame.required_bytes = 0;
    frame.required_resources = 0;
    return NA_STATUS_OK;
}

void invocation_state::restore_result(freelibcxx::vector<byte> &&bytes, capability::transfer_record_list &&resources)
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (!result_claimed_ || phase_ != invocation_phase::ready)
        return;
    response_bytes_ = std::move(bytes);
    response_resources_ = std::move(resources);
    result_claimed_ = false;
}

na_status_t invocation_state::commit_result()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (!result_claimed_ || phase_ != invocation_phase::ready)
        return phase_ == invocation_phase::consumed ? NA_STATUS_ALREADY_CONSUMED : NA_STATUS_WOULD_BLOCK;
    phase_ = invocation_phase::consumed;
    result_claimed_ = false;
    response_bytes_.clear();
    response_resources_.clear();
    release_result_budget_locked();
    wake_invocation_waiters();
    return NA_STATUS_OK;
}

invocation_object::invocation_object(handle_t<invocation_state> state)
    : kobject(type_e::invocation)
    , state_(std::move(state))
{
}

invocation_object::~invocation_object()
{
    if (state_)
        state_->close_client();
}

void invocation_object::on_capability_release(capability::location where)
{
    if (where == capability::location::table_root && state_)
        state_->close_client();
}

na_signal_t invocation_object::capability_signals() const { return state_ ? state_->signals() : 0; }

u64 invocation_object::capability_state() const { return state_ ? static_cast<u64>(state_->signals()) : 0; }

responder_object::responder_object(handle_t<invocation_state> state)
    : kobject(type_e::responder)
    , state_(std::move(state))
    , consumed_(false)
{
}

responder_object::~responder_object()
{
    if (!consumed_ && state_)
        state_->abandon_responder();
}

void responder_object::on_capability_release(capability::location where)
{
    if (where == capability::location::table_root && !consumed_ && state_)
        state_->abandon_responder();
}

na_signal_t responder_object::capability_signals() const { return state_ ? state_->signals() : 0; }

u64 responder_object::capability_state() const { return state_ ? static_cast<u64>(state_->signals()) : 0; }

protocol_state::protocol_state(const na_protocol_descriptor_t &descriptor, u64 max_messages, u64 max_bytes,
                               u64 max_resources)
    : descriptor_(descriptor)
    , queue_(nullptr)
    , max_messages_(max_messages)
    , max_bytes_(max_bytes)
    , max_resources_(max_resources)
    , owners_{0, 0}
    , roots_{0, 0}
    , active_operations_(0)
    , active_claims_(0)
    , valid_(false)
    , server_closed_(false)
{
    auto **storage = reinterpret_cast<invocation_request **>(memory::KernelCommonAllocatorV->allocate(
        sizeof(invocation_request *) * max_messages_, alignof(invocation_request *)));
    if (storage != nullptr)
        memset(storage, 0, sizeof(invocation_request *) * max_messages_);
    queue_ = memory::New<queue>(memory::KernelCommonAllocatorV, storage, max_messages_);
    valid_ = queue_ != nullptr && storage != nullptr;
}

protocol_state::~protocol_state()
{
    if (queue_ == nullptr)
        return;
    while (!queue_->fifo.empty())
    {
        if (!queue_->fifo.claim_front())
            break;
        auto *request = queue_->fifo.front();
        queue_->fifo.commit_claim();
        if (request != nullptr)
        {
            release_protocol_global(request->bytes.size(), request->queued_resource_count);
            request->state->clear_queue_owner();
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
        }
    }
    if (queue_->storage != nullptr)
        memory::KernelCommonAllocatorV->deallocate(queue_->storage);
    memory::Delete<>(memory::KernelCommonAllocatorV, queue_);
}

na_signal_t protocol_state::signals(endpoint_role role) const
{
    if (!valid_ || queue_ == nullptr)
        return 0;
    uctx::RawSpinLockUninterruptibleContext guard(const_cast<lock::spinlock_t &>(lock_));
    const u8 index = role == endpoint_role::client ? 0 : 1;
    const u8 peer = 1 - index;
    na_signal_t result = 0;
    if (role == endpoint_role::server && !queue_->fifo.empty())
        result |= NA_SIGNAL_READABLE;
    if (role == endpoint_role::client && owners_[peer].load() != 0 && !queue_->fifo.full() &&
        queue_->bytes < max_bytes_ && queue_->resources < max_resources_)
        result |= NA_SIGNAL_WRITABLE;
    if (owners_[peer].load() == 0 || (role == endpoint_role::client && server_closed_))
        result |= NA_SIGNAL_PEER_CLOSED;
    return result;
}

na_status_t protocol_state::enqueue(invocation_request *request, bool *queued)
{
    if (queued != nullptr)
        *queued = false;
    if (!valid_ || queue_ == nullptr || request == nullptr || request->bytes.size() > max_bytes_ ||
        request->resources.size() > max_resources_)
        return NA_STATUS_INVALID_MESSAGE;
    na_status_t result = NA_STATUS_OK;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (owners_[1].load() == 0 || server_closed_)
            result = NA_STATUS_PEER_CLOSED;
        else
        {
            const u64 resource_count = request->resources.size() + (request->responder.valid() ? 1 : 0);
            if (resource_count > max_resources_)
                result = NA_STATUS_INVALID_MESSAGE;
            else if (queue_->fifo.full() || queue_->bytes > max_bytes_ - request->bytes.size() ||
                     queue_->resources > max_resources_ - resource_count)
                result = NA_STATUS_WOULD_BLOCK;
            else if (!reserve_protocol_global(request->bytes.size(), resource_count))
                result = NA_STATUS_RESOURCE_EXHAUSTED;
            else if (!queue_->fifo.try_push(request))
            {
                release_protocol_global(request->bytes.size(), resource_count);
                result = NA_STATUS_WOULD_BLOCK;
            }
            else
            {
                request->queued_resource_count = resource_count;
                queue_->bytes += request->bytes.size();
                queue_->resources += resource_count;
                if (queued != nullptr)
                    *queued = true;
            }
        }
    }
    if (result == NA_STATUS_OK)
    {
        auto state = request->state;
        if (state->cancellation_requested())
            state->cancel(this);
        if (queued != nullptr && *queued && (state->signals() & NA_SIGNAL_COMPLETED) != 0)
        {
            auto *removed = remove_queued(state.operator&());
            if (removed != nullptr)
            {
                removed->state->clear_queue_owner();
                memory::Delete<>(memory::KernelCommonAllocatorV, removed);
                *queued = false;
            }
        }
        wake_invocation_waiters();
    }
    return result;
}

invocation_request *protocol_state::remove_queued(invocation_state *state)
{
    if (!valid_ || queue_ == nullptr || state == nullptr)
        return nullptr;

    invocation_request *removed = nullptr;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        for (u64 i = 0; i < queue_->fifo.size(); i++)
        {
            auto *request = queue_->fifo.at(i);
            if (request == nullptr || request->state.operator&() != state)
                continue;
            if (!queue_->fifo.remove(request))
                return nullptr;
            queue_->bytes -= request->bytes.size();
            queue_->resources -= request->queued_resource_count;
            release_protocol_global(request->bytes.size(), request->queued_resource_count);
            removed = request;
            break;
        }
    }
    if (removed != nullptr)
        wake_invocation_waiters();
    return removed;
}

na_status_t protocol_state::claim(invocation_request *&request)
{
    request = nullptr;
    if (!valid_ || queue_ == nullptr)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (queue_->fifo.empty())
        return owners_[0].load() == 0 ? NA_STATUS_PEER_CLOSED : NA_STATUS_WOULD_BLOCK;
    if (!queue_->fifo.claim_front())
        return NA_STATUS_WOULD_BLOCK;
    request = queue_->fifo.front();
    active_claims_.fetch_add(1);
    return NA_STATUS_OK;
}

bool protocol_state::cancel_claim(invocation_request *request)
{
    if (!valid_ || queue_ == nullptr)
        return false;
    bool cancelled = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!queue_->fifo.is_claimed() || queue_->fifo.front() != request)
            return false;
        queue_->fifo.cancel_claim();
        active_claims_.fetch_sub(1);
        cancelled = true;
    }
    if (cancelled)
        wake_invocation_waiters();
    return cancelled;
}

bool protocol_state::commit_claim(invocation_request *request)
{
    if (!valid_ || queue_ == nullptr)
        return false;
    bool committed = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!queue_->fifo.is_claimed() || queue_->fifo.front() != request)
            return false;
        queue_->fifo.commit_claim();
        const u64 resource_count = request->queued_resource_count;
        queue_->bytes -= request->bytes.size();
        queue_->resources -= resource_count;
        active_claims_.fetch_sub(1);
        release_protocol_global(request->bytes.size(), resource_count);
        committed = true;
    }
    if (committed)
        wake_invocation_waiters();
    return committed;
}

bool protocol_state::abort_claim(invocation_request *request)
{
    if (!valid_ || queue_ == nullptr)
        return false;
    bool aborted = false;
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (!queue_->fifo.is_claimed() || queue_->fifo.front() != request)
            return false;
        queue_->fifo.commit_claim();
        queue_->bytes -= request->bytes.size();
        queue_->resources -= request->queued_resource_count;
        active_claims_.fetch_sub(1);
        release_protocol_global(request->bytes.size(), request->queued_resource_count);
        aborted = true;
    }
    if (aborted)
        wake_invocation_waiters();
    return aborted;
}

void protocol_state::endpoint_acquired(endpoint_role role, capability::location where)
{
    const u8 index = role == endpoint_role::client ? 0 : 1;
    owners_[index].fetch_add(1);
    if (where == capability::location::table_root)
        roots_[index].fetch_add(1);
}

void protocol_state::endpoint_released(endpoint_role role, capability::location where)
{
    const u8 index = role == endpoint_role::client ? 0 : 1;
    owners_[index].fetch_sub(1);
    if (where == capability::location::table_root)
        roots_[index].fetch_sub(1);
    if (role == endpoint_role::server && owners_[index].load() == 0)
        close_server_queue();
}

void protocol_state::begin_operation() { active_operations_.fetch_add(1); }

void protocol_state::end_operation() { active_operations_.fetch_sub(1); }

void protocol_state::close_server_queue()
{
    freelibcxx::vector<invocation_request *> discarded(memory::KernelCommonAllocatorV);
    {
        uctx::RawSpinLockUninterruptibleContext guard(lock_);
        if (queue_ == nullptr)
            return;
        server_closed_ = true;
        while (!queue_->fifo.empty())
        {
            if (!queue_->fifo.claim_front())
                break;
            auto *request = queue_->fifo.front();
            queue_->fifo.commit_claim();
            if (request != nullptr)
            {
                queue_->bytes -= request->bytes.size();
                queue_->resources -= request->queued_resource_count;
                release_protocol_global(request->bytes.size(), request->queued_resource_count);
                discarded.push_back(request);
            }
        }
    }
    for (auto *request : discarded)
    {
        request->state->clear_queue_owner();
        request->state->complete_not_delivered(NA_OUTCOME_REASON_PEER_CLOSED);
        memory::Delete<>(memory::KernelCommonAllocatorV, request);
    }
    wake_invocation_waiters();
}

void protocol_state::protocol_violation() { close_server_queue(); }

void notify_invocation_waiters() { wake_invocation_waiters(); }

na_status_t create_protocol_descriptor(task::resource_table_t &resources, const na_protocol_descriptor_t *input,
                                       na_handle_t *output)
{
    if (!valid_user_output(output))
        return NA_STATUS_FAULT;
    na_protocol_descriptor_t descriptor{};
    auto status = copy_frame(descriptor, input);
    if (status != NA_STATUS_OK)
        return status;
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(input), sizeof(*input), reinterpret_cast<u64>(output),
                                       sizeof(*output)))
        return NA_STATUS_INVALID_ARGUMENT;
    status = validate_descriptor(descriptor);
    if (status != NA_STATUS_OK)
        return status;
    auto object = handle_t<protocol_descriptor>::make(descriptor);
    capability::metadata metadata;
    metadata.binding = NA_BINDING_NONE;
    metadata.protocol_uuid = descriptor.uuid;
    metadata.scope = descriptor.scope;
    metadata.revision = descriptor.revision;
    metadata.features = descriptor.features;
    metadata.meta_rights = NA_RIGHT_DUPLICATE | NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT;
    metadata.protocol_rights = descriptor.protocol_rights;
    const na_handle_t handle = resources.install_native(std::move(object), metadata);
    if (handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const auto copy_status = naos::usercopy::copy_to(reinterpret_cast<u64>(output), &handle, sizeof(handle));
    if (copy_status != NA_STATUS_OK)
        resources.close_native(handle);
    return copy_status;
}

na_status_t create_protocol_endpoint(task::resource_table_t &resources, na_handle_t descriptor_handle,
                                     const na_protocol_endpoint_options_t *options, na_handle_t *client,
                                     na_handle_t *server)
{
    if (!valid_user_output(client) || !valid_user_output(server))
        return NA_STATUS_FAULT;
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(client), sizeof(*client), reinterpret_cast<u64>(server),
                                       sizeof(*server)))
        return NA_STATUS_INVALID_ARGUMENT;
    capability::entry descriptor_entry;
    if (!resources.lookup_native(descriptor_handle, descriptor_entry) || !descriptor_entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if ((descriptor_entry.meta.meta_rights & NA_RIGHT_INSPECT) == 0)
        return NA_STATUS_ACCESS_DENIED;
    auto *descriptor = descriptor_entry.object->get<protocol_descriptor>();
    if (descriptor == nullptr)
        return NA_STATUS_WRONG_BINDING;

    na_protocol_endpoint_options_t values{};
    values.struct_size = sizeof(values);
    values.client_meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    values.server_meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    values.client_protocol_rights = descriptor->descriptor().protocol_rights;
    values.server_protocol_rights = descriptor->descriptor().protocol_rights;
    values.max_messages = 64;
    values.max_bytes = descriptor->descriptor().max_request_bytes == 0 ? NA_CHANNEL_DEFAULT_MAX_BYTES
                                                                       : descriptor->descriptor().max_request_bytes * 4;
    values.max_resources = descriptor->descriptor().max_resources == 0 ? NA_CHANNEL_DEFAULT_MAX_RESOURCES
                                                                       : descriptor->descriptor().max_resources;
    if (options != nullptr)
    {
        auto status = copy_frame(values, options);
        if (status != NA_STATUS_OK)
            return status;
        if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0)
            return NA_STATUS_INVALID_ARGUMENT;
        if (values.max_messages == 0)
            values.max_messages = 64;
        if (values.max_bytes == 0)
            values.max_bytes = NA_CHANNEL_DEFAULT_MAX_BYTES;
        if (values.max_resources == 0)
            values.max_resources = NA_CHANNEL_DEFAULT_MAX_RESOURCES;
    }
    if (options != nullptr && (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(options), sizeof(*options),
                                                              reinterpret_cast<u64>(client), sizeof(*client)) ||
                               naos::usercopy::ranges_overlap(reinterpret_cast<u64>(options), sizeof(*options),
                                                              reinterpret_cast<u64>(server), sizeof(*server))))
        return NA_STATUS_INVALID_ARGUMENT;
    if (values.client_protocol_rights == 0)
        values.client_protocol_rights = descriptor->descriptor().protocol_rights;
    if (values.server_protocol_rights == 0)
        values.server_protocol_rights = descriptor->descriptor().protocol_rights;
    if ((values.client_protocol_rights & ~descriptor->descriptor().protocol_rights) != 0 ||
        (values.server_protocol_rights & ~descriptor->descriptor().protocol_rights) != 0)
        return NA_STATUS_ACCESS_DENIED;
    if (values.max_messages > NA_CHANNEL_MAX_MESSAGES || values.max_bytes > max_kernel_payload * 16 ||
        values.max_resources > NA_CHANNEL_MAX_RESOURCES ||
        (values.client_meta_rights & ~((na_meta_rights_t)(NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT))) !=
            0 ||
        (values.server_meta_rights & ~((na_meta_rights_t)(NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT))) != 0)
        return NA_STATUS_INVALID_ARGUMENT;

    auto state = handle_t<protocol_state>::make(descriptor->descriptor(), values.max_messages, values.max_bytes,
                                                values.max_resources);
    if (!state || !state->valid() || state->descriptor().scope == NA_SCOPE_NONE)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    auto client_object = handle_t<protocol_endpoint>::make(state, endpoint_role::client);
    auto server_object = handle_t<protocol_endpoint>::make(state, endpoint_role::server);
    if (!client_object || !server_object)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    capability::metadata client_metadata =
        kernel_view_metadata(descriptor->descriptor().scope, descriptor->descriptor().uuid, values.client_meta_rights,
                             values.client_protocol_rights);
    client_metadata.binding = NA_BINDING_CLIENT_END;
    capability::metadata server_metadata = client_metadata;
    server_metadata.binding = NA_BINDING_SERVER_END;
    client_metadata.protocol_rights |= NA_PROTOCOL_RIGHT_INVOKE;
    server_metadata.protocol_rights = values.server_protocol_rights | NA_PROTOCOL_RIGHT_INVOKE;
    const na_handle_t client_handle = resources.install_native(std::move(client_object), client_metadata);
    if (client_handle == NA_HANDLE_INVALID)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    const na_handle_t server_handle = resources.install_native(std::move(server_object), server_metadata);
    if (server_handle == NA_HANDLE_INVALID)
    {
        resources.close_native(client_handle);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    if (naos::usercopy::copy_to(reinterpret_cast<u64>(client), &client_handle, sizeof(client_handle)) != NA_STATUS_OK ||
        naos::usercopy::copy_to(reinterpret_cast<u64>(server), &server_handle, sizeof(server_handle)) != NA_STATUS_OK)
    {
        resources.close_native(client_handle);
        resources.close_native(server_handle);
        return NA_STATUS_FAULT;
    }
    return NA_STATUS_OK;
}

na_status_t invoke_submit(task::resource_table_t &resources, na_handle_t target_handle, const na_submit_frame_t *frame,
                          na_handle_t *invocation, bool oneway)
{
    na_submit_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    status = validate_submit_frame(values, oneway);
    if (status != NA_STATUS_OK)
        return status;
    if (!oneway && !valid_user_output(invocation))
        return NA_STATUS_FAULT;
    if (oneway && invocation != nullptr)
        return NA_STATUS_INVALID_ARGUMENT;
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.request,
                                       values.request_bytes) ||
        naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                       values.resource_count * sizeof(na_resource_disposition_t)) ||
        naos::usercopy::ranges_overlap(values.request, values.request_bytes, values.resources,
                                       values.resource_count * sizeof(na_resource_disposition_t)))
        return NA_STATUS_INVALID_ARGUMENT;

    capability::entry target;
    if (!resources.lookup_native(target_handle, target) || !target.object)
        return NA_STATUS_INVALID_HANDLE;
    if ((target.meta.meta_rights & NA_RIGHT_TRANSFER) == 0 && values.resource_count != 0)
        return NA_STATUS_ACCESS_DENIED;

    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    freelibcxx::vector<na_resource_disposition_t> dispositions(memory::KernelCommonAllocatorV);
    status = snapshot_request(values, bytes, dispositions);
    if (status != NA_STATUS_OK)
        return status;

    protocol_endpoint *client_endpoint = nullptr;
    const bool is_client = endpoint_is_client(target, client_endpoint);
    const bool is_kernel = target.meta.binding == NA_BINDING_KERNEL_VIEW;
    if (!is_client && !is_kernel)
        return NA_STATUS_WRONG_BINDING;
    if ((target.meta.protocol_rights & NA_PROTOCOL_RIGHT_INVOKE) == 0)
        return NA_STATUS_ACCESS_DENIED;
    if (is_kernel && values.method_id == 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (is_client && (client_endpoint->state() == nullptr || !client_endpoint->state()->valid()))
        return NA_STATUS_OBJECT_REVOKED;
    if (is_client && !descriptor_allows_method(client_endpoint->state()->descriptor(), values.method_id))
        return NA_STATUS_INVALID_MESSAGE;
    if (is_client)
    {
        const auto &descriptor = client_endpoint->state()->descriptor();
        if ((descriptor.max_request_bytes != 0 && values.request_bytes > descriptor.max_request_bytes) ||
            (descriptor.max_resources != 0 && values.resource_count > descriptor.max_resources))
            return NA_STATUS_INVALID_MESSAGE;
    }
    if (oneway)
    {
        if (!is_client || (client_endpoint->state()->descriptor().flags & NA_PROTOCOL_FLAG_ALLOW_ONEWAY) == 0 ||
            !descriptor_allows_oneway(client_endpoint->state()->descriptor(), values.method_id))
            return NA_STATUS_ACCESS_DENIED;
    }
    else if (is_client && descriptor_allows_oneway(client_endpoint->state()->descriptor(), values.method_id))
        return NA_STATUS_INVALID_ARGUMENT;

    // Allocate the Invocation entry before admission, but publish its opaque
    // value to user memory only after the request has been accepted.
    u64 max_response_bytes = max_kernel_payload;
    u64 max_response_resources = NA_CHANNEL_MAX_RESOURCES;
    if (is_client)
    {
        max_response_bytes = client_endpoint->state()->descriptor().max_response_bytes;
        max_response_resources = client_endpoint->state()->descriptor().max_resources;
    }
    handle_t<invocation_state> state = handle_t<invocation_state>::make(values.method_id, values.operation_budget,
                                                                        max_response_bytes, max_response_resources);
    if (!state)
        return NA_STATUS_RESOURCE_EXHAUSTED;
    if (!oneway && !state->reserve_result_budget())
        return NA_STATUS_RESOURCE_EXHAUSTED;
    status = state->arm_deadline(state);
    if (status != NA_STATUS_OK)
        return status;
    if (is_client)
        state->set_queue_owner(client_endpoint->state_ref());

    na_handle_t invocation_handle = NA_HANDLE_INVALID;
    capability::metadata invocation_metadata;
    invocation_metadata.binding = NA_BINDING_INVOCATION;
    invocation_metadata.protocol_uuid = target.meta.protocol_uuid;
    invocation_metadata.scope = target.meta.scope;
    invocation_metadata.revision = target.meta.revision;
    invocation_metadata.features = target.meta.features;
    invocation_metadata.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    handle_t<invocation_object> invocation_object_handle;
    if (!oneway)
    {
        invocation_object_handle = handle_t<invocation_object>::make(state);
        if (!invocation_object_handle)
            return NA_STATUS_RESOURCE_EXHAUSTED;
        invocation_handle = resources.install_native(std::move(invocation_object_handle), invocation_metadata);
        if (invocation_handle == NA_HANDLE_INVALID)
            return NA_STATUS_RESOURCE_EXHAUSTED;
    }

    if (is_client)
    {
        auto *request = memory::New<invocation_request>(memory::KernelCommonAllocatorV, memory::MemoryAllocatorV, state,
                                                        values.method_id, state->operation_deadline());
        if (request == nullptr)
        {
            if (!oneway)
                resources.close_native(invocation_handle);
            return NA_STATUS_RESOURCE_EXHAUSTED;
        }
        if (!oneway)
        {
            auto responder = handle_t<responder_object>::make(state);
            if (!responder)
            {
                memory::Delete<>(memory::KernelCommonAllocatorV, request);
                resources.close_native(invocation_handle);
                return NA_STATUS_RESOURCE_EXHAUSTED;
            }
            capability::metadata responder_metadata = invocation_metadata;
            responder_metadata.binding = NA_BINDING_RESPONDER;
            responder_metadata.meta_rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
            request->responder = capability::transferred_resource(std::move(responder), responder_metadata);
        }
        capability::transfer_record_list records(memory::KernelCommonAllocatorV);
        status = resources.take_native_batch(dispositions.data(), dispositions.size(), target_handle, records);
        if (status != NA_STATUS_OK)
        {
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            if (!oneway)
                resources.close_native(invocation_handle);
            return status;
        }
        request->bytes = std::move(bytes);
        request->resources = std::move(records);
        bool queued = false;
        status = client_endpoint->state()->enqueue(request, &queued);
        (void)queued;
        if (status != NA_STATUS_OK)
        {
            resources.restore_native_batch(request->resources);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            if (!oneway)
                resources.close_native(invocation_handle);
            return status;
        }
        if (!oneway)
        {
            status = naos::usercopy::copy_to(reinterpret_cast<u64>(invocation), &invocation_handle,
                                             sizeof(invocation_handle));
            if (status != NA_STATUS_OK)
            {
                resources.close_native(invocation_handle);
                return status;
            }
        }
        return NA_STATUS_OK;
    }

    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    status = resources.take_native_batch(dispositions.data(), dispositions.size(), target_handle, records);
    if (status != NA_STATUS_OK)
    {
        resources.restore_native_batch(records);
        if (!oneway)
            resources.close_native(invocation_handle);
        return status;
    }

    status = dispatch_kernel_view(target, *state, values.method_id, bytes, records);
    resources.restore_native_batch(records);
    if (status == NA_STATUS_NOT_SUPPORTED)
    {
        state->complete_reply(empty_bytes(), empty_resources(), ENOTSUP);
        status = NA_STATUS_OK;
    }
    else if (status != NA_STATUS_OK)
    {
        state->complete_reply(empty_bytes(), empty_resources(), EINVAL);
        status = NA_STATUS_OK;
    }
    if (status == NA_STATUS_OK && !oneway)
    {
        status =
            naos::usercopy::copy_to(reinterpret_cast<u64>(invocation), &invocation_handle, sizeof(invocation_handle));
        if (status != NA_STATUS_OK)
        {
            resources.close_native(invocation_handle);
            return status;
        }
    }
    if (status != NA_STATUS_OK && !oneway)
        resources.close_native(invocation_handle);
    return status;
}

na_status_t receive_protocol(task::resource_table_t &resources, na_handle_t endpoint_handle,
                             na_channel_receive_frame_t *frame)
{
    na_channel_receive_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (values.byte_capacity > max_kernel_payload || values.resource_capacity > NA_CHANNEL_MAX_RESOURCES)
        return NA_STATUS_INVALID_ARGUMENT;

    capability::entry endpoint_entry;
    if (!resources.lookup_native(endpoint_handle, endpoint_entry) || !endpoint_entry.object)
        return NA_STATUS_INVALID_HANDLE;
    protocol_endpoint *endpoint = nullptr;
    if (!endpoint_is_server(endpoint_entry, endpoint))
        return NA_STATUS_WRONG_BINDING;
    endpoint->begin_operation();
    auto finish = [&](na_status_t result) {
        endpoint->end_operation();
        return result;
    };

    for (;;)
    {
        invocation_request *request = nullptr;
        status = endpoint->state()->claim(request);
        if (status != NA_STATUS_OK)
            return finish(status);
        if (request->state->deadline_expired(false) || request->state->signals() & NA_SIGNAL_COMPLETED)
        {
            endpoint->state()->commit_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            continue;
        }
        const auto &descriptor = endpoint->state()->descriptor();
        const bool method_valid = descriptor_allows_method(descriptor, request->method_id);
        const bool request_size_valid =
            descriptor.max_request_bytes == 0 || request->bytes.size() <= descriptor.max_request_bytes;
        const bool resource_count_valid =
            descriptor.max_resources == 0 || request->resources.size() <= descriptor.max_resources;
        const bool oneway = descriptor_allows_oneway(descriptor, request->method_id);
        const bool responder_valid = request->responder.valid();
        if (!method_valid || !request_size_valid || !resource_count_valid || oneway == responder_valid)
        {
            request->state->complete_failure(NA_EXECUTION_NOT_DELIVERED, NA_OUTCOME_REASON_PROTOCOL_VIOLATION);
            endpoint->state()->commit_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            endpoint->state()->protocol_violation();
            return finish(NA_STATUS_INVALID_MESSAGE);
        }
        values.method_id = request->method_id;
        values.required_bytes = request->bytes.size();
        values.required_resources = request->resources.size();
        values.actual_bytes = 0;
        values.actual_resources = 0;
        values.responder = NA_HANDLE_INVALID;
        if (values.byte_capacity < request->bytes.size() || values.resource_capacity < request->resources.size())
        {
            const auto write_status = write_frame(frame, values);
            endpoint->state()->cancel_claim(request);
            return finish(write_status == NA_STATUS_OK ? NA_STATUS_BUFFER_TOO_SMALL : write_status);
        }
        if (!naos::usercopy::valid_output_range(values.bytes, values.byte_capacity) ||
            !naos::usercopy::valid_output_range(values.resources, values.resource_capacity * sizeof(na_handle_t)))
        {
            endpoint->state()->cancel_claim(request);
            return finish(NA_STATUS_FAULT);
        }
        if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.bytes,
                                           request->bytes.size()) ||
            naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                           request->resources.size() * sizeof(na_handle_t)) ||
            naos::usercopy::ranges_overlap(values.bytes, request->bytes.size(), values.resources,
                                           request->resources.size() * sizeof(na_handle_t)))
        {
            endpoint->state()->cancel_claim(request);
            return finish(NA_STATUS_INVALID_ARGUMENT);
        }

        // Claiming the queue head is not itself the dispatch linearization
        // point: a capacity/fault path must leave the request available. Once
        // this transition succeeds, cancellation can only be observed by the
        // handler and cannot publish NOT_DELIVERED behind the receiver's back.
        if (!request->state->begin_receive())
        {
            endpoint->state()->commit_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            continue;
        }
        auto rollback_receive = [&] {
            request->state->rollback_receive();
            endpoint->state()->cancel_claim(request);
        };

        freelibcxx::vector<na_handle_t> reserved(memory::KernelCommonAllocatorV);
        status = resources.reserve_native(reserved, request->resources.size() + (request->responder.valid() ? 1 : 0));
        if (status != NA_STATUS_OK)
        {
            rollback_receive();
            return finish(status);
        }
        status = naos::usercopy::copy_to(values.bytes, request->bytes.data(), request->bytes.size());
        if (status == NA_STATUS_OK && !request->resources.empty())
            status = naos::usercopy::copy_to(values.resources, reserved.data(),
                                             request->resources.size() * sizeof(na_handle_t));
        if (status != NA_STATUS_OK)
        {
            resources.rollback_native(reserved);
            rollback_receive();
            return finish(status);
        }
        values.actual_bytes = request->bytes.size();
        values.actual_resources = request->resources.size();
        values.required_bytes = 0;
        values.required_resources = 0;
        if (request->responder.valid())
            values.responder = reserved[reserved.size() - 1];
        status = write_frame(frame, values);
        if (status != NA_STATUS_OK)
        {
            resources.rollback_native(reserved);
            rollback_receive();
            return finish(status);
        }
        for (u64 i = 0; i < request->resources.size(); i++)
        {
            status = resources.activate_native(reserved[i], std::move(request->resources[i].resource));
            if (status != NA_STATUS_OK)
                break;
        }
        if (status == NA_STATUS_OK && request->responder.valid())
            status = resources.activate_native(reserved[reserved.size() - 1], std::move(request->responder));
        if (status != NA_STATUS_OK)
        {
            for (auto handle : reserved)
                resources.close_native(handle);
            resources.rollback_native(reserved);
            rollback_receive();
            return finish(status);
        }
        if (!request->state->finish_dispatch())
        {
            for (auto handle : reserved)
                resources.close_native(handle);
            endpoint->state()->abort_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            return finish(NA_STATUS_PEER_CLOSED);
        }
        if (!endpoint->state()->commit_claim(request))
        {
            for (auto handle : reserved)
                resources.close_native(handle);
            request->state->complete_failure(NA_EXECUTION_OUTCOME_UNKNOWN, NA_OUTCOME_REASON_BROKER_FAILURE);
            endpoint->state()->abort_claim(request);
            memory::Delete<>(memory::KernelCommonAllocatorV, request);
            return finish(NA_STATUS_WOULD_BLOCK);
        }
        memory::Delete<>(memory::KernelCommonAllocatorV, request);
        return finish(NA_STATUS_OK);
    }
}

na_status_t invocation_cancel(task::resource_table_t &resources, na_handle_t invocation_handle)
{
    capability::entry entry;
    if (!resources.lookup_native(invocation_handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_INVOCATION)
        return NA_STATUS_WRONG_BINDING;
    auto *invocation = entry.object->get<invocation_object>();
    if (invocation == nullptr)
        return NA_STATUS_WRONG_BINDING;
    return invocation->state()->cancel(nullptr) ? NA_STATUS_OK : NA_STATUS_ALREADY_CONSUMED;
}

na_status_t invocation_take_result(task::resource_table_t &resources, na_handle_t invocation_handle,
                                   na_result_frame_t *frame)
{
    na_result_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0)
        return NA_STATUS_INVALID_ARGUMENT;
    if (values.byte_capacity > max_kernel_payload || values.resource_capacity > NA_CHANNEL_MAX_RESOURCES)
        return NA_STATUS_INVALID_ARGUMENT;

    capability::entry entry;
    if (!resources.lookup_native(invocation_handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_INVOCATION)
        return NA_STATUS_WRONG_BINDING;
    auto *invocation = entry.object->get<invocation_object>();
    if (invocation == nullptr)
        return NA_STATUS_WRONG_BINDING;

    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    status = invocation->state()->claim_result(values, bytes, records);
    if (status != NA_STATUS_OK)
    {
        write_frame(frame, values);
        return status;
    }
    if (!naos::usercopy::valid_output_range(values.bytes, values.byte_capacity) ||
        !naos::usercopy::valid_output_range(values.resources, values.resource_capacity * sizeof(na_handle_t)))
    {
        invocation->state()->restore_result(std::move(bytes), std::move(records));
        return NA_STATUS_FAULT;
    }
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.bytes, bytes.size()) ||
        naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                       records.size() * sizeof(na_handle_t)) ||
        naos::usercopy::ranges_overlap(values.bytes, bytes.size(), values.resources,
                                       records.size() * sizeof(na_handle_t)))
    {
        invocation->state()->restore_result(std::move(bytes), std::move(records));
        return NA_STATUS_INVALID_ARGUMENT;
    }

    freelibcxx::vector<na_handle_t> reserved(memory::KernelCommonAllocatorV);
    status = resources.reserve_native(reserved, records.size());
    if (status == NA_STATUS_OK)
        status = naos::usercopy::copy_to(values.bytes, bytes.data(), bytes.size());
    if (status == NA_STATUS_OK && !records.empty())
        status = naos::usercopy::copy_to(values.resources, reserved.data(), records.size() * sizeof(na_handle_t));
    values.actual_bytes = bytes.size();
    values.actual_resources = records.size();
    values.required_bytes = 0;
    values.required_resources = 0;
    if (status == NA_STATUS_OK)
        status = write_frame(frame, values);
    if (status != NA_STATUS_OK)
    {
        resources.rollback_native(reserved);
        invocation->state()->restore_result(std::move(bytes), std::move(records));
        return status;
    }
    for (u64 i = 0; i < records.size(); i++)
    {
        status = resources.activate_native(reserved[i], std::move(records[i].resource));
        if (status != NA_STATUS_OK)
            break;
    }
    if (status != NA_STATUS_OK)
    {
        for (auto handle : reserved)
            resources.close_native(handle);
        resources.rollback_native(reserved);
        invocation->state()->restore_result(std::move(bytes), std::move(records));
        return status;
    }
    status = invocation->state()->commit_result();
    if (status != NA_STATUS_OK)
    {
        for (auto handle : reserved)
            resources.close_native(handle);
        return status;
    }
    return NA_STATUS_OK;
}

na_status_t responder_reply(task::resource_table_t &resources, na_handle_t responder_handle,
                            const na_reply_frame_t *frame)
{
    na_reply_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0 ||
        values.reserved1 != 0 || values.byte_count > max_kernel_payload ||
        values.resource_count > NA_CHANNEL_MAX_RESOURCES)
        return NA_STATUS_INVALID_ARGUMENT;
    u64 resources_bytes = 0;
    if (!checked_multiply(values.resource_count, sizeof(na_resource_disposition_t), resources_bytes) ||
        !naos::usercopy::valid_range(values.bytes, values.byte_count) ||
        !naos::usercopy::valid_range(values.resources, resources_bytes))
        return NA_STATUS_FAULT;
    if (naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.bytes, values.byte_count) ||
        naos::usercopy::ranges_overlap(reinterpret_cast<u64>(frame), sizeof(*frame), values.resources,
                                       resources_bytes) ||
        naos::usercopy::ranges_overlap(values.bytes, values.byte_count, values.resources, resources_bytes))
        return NA_STATUS_INVALID_ARGUMENT;
    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    freelibcxx::vector<na_resource_disposition_t> dispositions(memory::KernelCommonAllocatorV);
    bytes.resize(values.byte_count, byte{});
    if (values.byte_count != 0)
    {
        status = naos::usercopy::copy_from(bytes.data(), values.bytes, values.byte_count);
        if (status != NA_STATUS_OK)
            return status;
    }
    dispositions.resize(values.resource_count, na_resource_disposition_t{});
    if (values.resource_count != 0)
    {
        status = naos::usercopy::copy_from(dispositions.data(), values.resources, resources_bytes);
        if (status != NA_STATUS_OK)
            return status;
    }

    capability::entry entry;
    if (!resources.lookup_native(responder_handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_RESPONDER)
        return NA_STATUS_WRONG_BINDING;
    auto *responder = entry.object->get<responder_object>();
    if (responder == nullptr)
        return NA_STATUS_WRONG_BINDING;
    if (!responder->state()->response_within_limits(bytes.size(), dispositions.size()))
        return NA_STATUS_INVALID_MESSAGE;
    capability::transfer_record_list records(memory::KernelCommonAllocatorV);
    status = resources.take_native_batch(dispositions.data(), dispositions.size(), responder_handle, records);
    if (status != NA_STATUS_OK)
        return status;
    if (!responder->state()->complete_reply(bytes, records))
    {
        resources.restore_native_batch(records);
        resources.close_native(responder_handle);
        return NA_STATUS_PEER_CLOSED;
    }
    responder->consume();
    resources.close_native(responder_handle);
    return NA_STATUS_OK;
}

na_status_t responder_fail(task::resource_table_t &resources, na_handle_t responder_handle,
                           const na_fail_frame_t *frame)
{
    na_fail_frame_t values{};
    auto status = copy_frame(values, frame);
    if (status != NA_STATUS_OK)
        return status;
    if (!valid_frame_size(values.struct_size, sizeof(values)) || values.flags != 0 || values.reserved0 != 0 ||
        values.reserved1 != 0 ||
        !valid_failure(static_cast<na_execution_outcome_t>(values.execution_outcome),
                       static_cast<na_outcome_reason_t>(values.outcome_reason)))
        return NA_STATUS_INVALID_ARGUMENT;
    capability::entry entry;
    if (!resources.lookup_native(responder_handle, entry) || !entry.object)
        return NA_STATUS_INVALID_HANDLE;
    if (entry.meta.binding != NA_BINDING_RESPONDER)
        return NA_STATUS_WRONG_BINDING;
    auto *responder = entry.object->get<responder_object>();
    if (responder == nullptr)
        return NA_STATUS_WRONG_BINDING;
    if (!responder->state()->complete_failure(static_cast<na_execution_outcome_t>(values.execution_outcome),
                                              static_cast<na_outcome_reason_t>(values.outcome_reason)))
    {
        resources.close_native(responder_handle);
        return NA_STATUS_PEER_CLOSED;
    }
    responder->consume();
    resources.close_native(responder_handle);
    return NA_STATUS_OK;
}

} // namespace naos::ipc
