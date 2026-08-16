#include "terminal_core.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <naos/abi.h>
#include <naos/generated/system/TerminalDriverControl_client.hpp>
#include <naos/generated/system/TerminalDriverFactory_client.hpp>
#include <naos/generated/system/TerminalJobControl.hpp>
#include <naos/generated/system/TerminalJobControl_client.hpp>
#include <naos/generated/system/TerminalManager.hpp>
#include <naos/generated/system/TerminalManager_server.hpp>
#include <naos/generated/system/TerminalMaster.hpp>
#include <naos/generated/system/TerminalMaster_server.hpp>
#include <naos/generated/system/TerminalSlave.hpp>
#include <naos/generated/system/TerminalSlave_server.hpp>
#include <naos/generated/system_uapi.h>
#include <naos/libnao.hpp>
#include <naos/outcome.hpp>
#include <naos/service_directory.hpp>
#include <naos/syscall.h>

[[gnu::weak]] void *__dso_handle;

extern "C" int naos_take_terminal_driver_factory(na_handle_t *handle);

namespace
{
struct service_state;
struct pty_pair;
struct pending_read;
struct pending_write;

naoidl::native_transport make_transport();
na_outcome_reason_t terminal_error_reason(int error);
int driver_result_error(const na_result_frame_t &result);
void reject_responder(na_handle_t responder, na_outcome_reason_t reason);
bool responder_cancelled_or_closed(na_handle_t responder);
void flush_driver_actions(service_state &state);
void flush_pending_creates(service_state &state);
void flush_pending_locator_opens(service_state &state);
void flush_pending_reads(service_state &state);
void flush_pending_writes(service_state &state);
void drop_pending_writes(service_state &state, na_handle_t endpoint);
void flush_pending_watches(service_state &state);
void ttyd_control_event(ttyd::control_event event, void *user_data);
na_handle_t public_job_control_handle(na_handle_t source);
na_handle_t restrict_terminal_client(na_handle_t client, uint64_t mode);
bool validate_response_resource(na_handle_t handle, uint64_t scope, const na_uuid_t &protocol_uuid, uint32_t binding);

constexpr uint64_t max_pairs = 256;
constexpr uint64_t max_endpoints = 4096;
constexpr uint64_t max_pending_per_binding = 8;
constexpr uint64_t clone_data_rights = 1 | 2;
// TerminalManager's mode word reserves bit 2 for the initial non-blocking
// state.  The same bit is used by the terminal open-description protocol.
constexpr uint64_t terminal_status_nonblock = 4;
constexpr uint64_t clone_share_open_description = 8;
constexpr uint64_t clone_initial_nonblock = 16;
constexpr uint64_t invalid_open_description = UINT64_MAX;

enum class driver_action_kind : std::uint64_t
{
    raise_foreground = 1,
    notify_winsize = 2,
    hangup = 3,
};

bool queue_driver_action(pty_pair &pair, driver_action_kind kind, std::uint64_t value,
                         na_handle_t responder = NA_HANDLE_INVALID);

struct pending_driver_action
{
    driver_action_kind kind = driver_action_kind::raise_foreground;
    std::uint64_t value = 0;
    na_handle_t responder = NA_HANDLE_INVALID;
    std::uint64_t sequence = 0;
};

struct pending_termios
{
    bool active = false;
    na_handle_t endpoint = NA_HANDLE_INVALID;
    na_handle_t responder = NA_HANDLE_INVALID;
    ttyd::termios attributes{};
    bool flush_input = false;
};

struct pty_pair
{
    bool allocated = false;
    uint64_t id = 0;
    uint64_t generation = 1;
    uint64_t factory_identity_id = 0;
    uint64_t factory_identity_generation = 0;
    std::array<std::uint8_t, 16> locator_token{};
    ttyd::terminal_core core;
    bool slave_locked = true;
    bool slave_granted = false;
    uint32_t slave_mode = 0620;
    bool revoked = false;
    bool retiring = false;
    na_handle_t job_control = NA_HANDLE_INVALID;
    na_handle_t driver_control = NA_HANDLE_INVALID;
    uint64_t open_bindings = 0;
    uint64_t master_bindings = 0;
    uint64_t slave_bindings = 0;
    pending_termios pending_attribute_change{};
    pending_driver_action pending_driver_actions[16]{};
    uint64_t pending_driver_action_count = 0;
    na_handle_t active_driver_invocation = NA_HANDLE_INVALID;
    driver_action_kind active_driver_action = driver_action_kind::raise_foreground;
    std::uint64_t active_driver_sequence = 0;
    std::uint64_t next_driver_sequence = 1;
    na_handle_t active_driver_responder = NA_HANDLE_INVALID;
    std::uint8_t active_driver_wire[256]{};
};

struct terminal_open_description
{
    bool allocated = false;
    uint64_t pair_id = 0;
    bool master = false;
    uint64_t status_flags = 0;
    uint64_t references = 0;
};

struct binding
{
    na_handle_t endpoint = NA_HANDLE_INVALID;
    uint64_t pair_id = 0;
    bool master = false;
    uint64_t mode = 3;
    uint64_t open_description = invalid_open_description;
};

struct pending_read
{
    na_handle_t endpoint = NA_HANDLE_INVALID;
    na_handle_t responder = NA_HANDLE_INVALID;
    std::size_t size = 0;
    bool master = false;
    uint64_t deadline_ms = 0;
    bool timer_started = false;
    bool nonblock = false;
};

struct pending_write
{
    na_handle_t endpoint = NA_HANDLE_INVALID;
    na_handle_t responder = NA_HANDLE_INVALID;
    std::uint8_t *data = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;
    bool master = false;
    uint64_t sequence = 0;
    bool nonblock = false;
};

struct pending_watch
{
    na_handle_t endpoint = NA_HANDLE_INVALID;
    na_handle_t responder = NA_HANDLE_INVALID;
    uint64_t mask = 0;
    uint64_t observed_generation = 0;
    bool master = false;
};

struct request_context
{
    na_handle_t responder = NA_HANDLE_INVALID;
    na_resource_disposition_t *response_resources = nullptr;
    uint64_t response_resource_count = 0;
    const na_handle_t *request_resources = nullptr;
    uint64_t request_resource_count = 0;
    uint64_t caller_pid = 0;
    naoidl::dispatch_outcome outcome = naoidl::dispatch_outcome::failed;
    na_outcome_reason_t failure_reason = NA_OUTCOME_REASON_REQUEST_DISCARDED;
    std::int64_t failure_error = 0;
};

enum class pending_create_kind : std::uint8_t
{
    pty,
    console,
};

enum class pending_locator_kind : std::uint8_t
{
    slave_locator,
    slave_by_number,
    controlling,
};

struct pending_locator_open
{
    bool active = false;
    na_handle_t invocation = NA_HANDLE_INVALID;
    na_handle_t responder = NA_HANDLE_INVALID;
    pending_locator_kind kind = pending_locator_kind::slave_locator;
    uint64_t pair_id = 0;
    uint64_t generation = 0;
    std::array<std::uint8_t, 16> token{};
    uint64_t mode = 0;
    uint64_t caller_pid = 0;
    std::uint8_t *wire = nullptr;
};

struct pending_create
{
    bool active = false;
    na_handle_t invocation = NA_HANDLE_INVALID;
    na_handle_t responder = NA_HANDLE_INVALID;
    naos::system::TerminalManager::create_pty_request request{};
    pending_create_kind kind = pending_create_kind::pty;
    bool console_master = false;
    uint64_t pair_id = 0;
    uint64_t generation = 0;
    std::uint8_t *wire = nullptr;
};

struct service_state
{
    na_handle_t listener_endpoint = NA_HANDLE_INVALID;
    na_handle_t endpoints[max_endpoints]{};
    uint64_t endpoint_count = 0;
    pty_pair pairs[max_pairs]{};
    uint64_t pair_count = 0;
    binding bindings[max_endpoints]{};
    uint64_t binding_count = 0;
    terminal_open_description open_descriptions[max_endpoints]{};
    pending_read pending_reads[max_endpoints]{};
    uint64_t pending_read_count = 0;
    pending_write pending_writes[max_endpoints]{};
    uint64_t pending_write_count = 0;
    pending_watch pending_watches[max_endpoints]{};
    uint64_t pending_watch_count = 0;
    na_handle_t master_descriptor = NA_HANDLE_INVALID;
    na_handle_t slave_descriptor = NA_HANDLE_INVALID;
    na_handle_t factory_handle = NA_HANDLE_INVALID;
    uint64_t next_pair_id = 1;
    uint64_t free_pair_ids[max_pairs]{};
    uint64_t free_pair_id_count = 0;
    uint64_t pair_generations[max_pairs + 1]{};
    uint64_t quota_rejections = 0;
    uint64_t next_write_sequence = 1;
    pending_create pending_creates[max_pairs]{};
    uint64_t pending_create_count = 0;
    pending_locator_open pending_locator_opens[max_pairs]{};
    uint64_t pending_locator_open_count = 0;
};

// In addition to service endpoints and downstream invocations, every retained
// responder is a wait source.  Otherwise a client cancelling an idle read or
// watch cannot wake ttyd to release the responder and its quota slot.
constexpr uint64_t max_wait_items = 6 * max_endpoints + 24 * max_pairs;

template <typename T> ttyd::termios to_ttyd_termios(const T &value)
{
    ttyd::termios result{};
    result.input_flags = value.input_flags;
    result.output_flags = value.output_flags;
    result.control_flags = value.control_flags;
    result.local_flags = value.local_flags;
    result.line = value.line;
    std::memcpy(result.control_chars, value.control_chars.data(), sizeof(result.control_chars));
    result.input_baud = value.input_baud;
    result.output_baud = value.output_baud;
    return result;
}

template <typename T> ttyd::winsize to_ttyd_winsize(const T &value)
{
    ttyd::winsize result{};
    result.rows = value.rows;
    result.columns = value.columns;
    result.x_pixels = value.x_pixels;
    result.y_pixels = value.y_pixels;
    return result;
}

template <typename T> T to_idl_termios(const ttyd::termios &value)
{
    T result{};
    result.input_flags = value.input_flags;
    result.output_flags = value.output_flags;
    result.control_flags = value.control_flags;
    result.local_flags = value.local_flags;
    result.line = value.line;
    std::memcpy(result.control_chars.data(), value.control_chars, sizeof(value.control_chars));
    result.input_baud = value.input_baud;
    result.output_baud = value.output_baud;
    return result;
}

template <typename T> T to_idl_winsize(const ttyd::winsize &value)
{
    T result{};
    result.rows = value.rows;
    result.columns = value.columns;
    result.x_pixels = value.x_pixels;
    result.y_pixels = value.y_pixels;
    return result;
}

pty_pair *find_pair(service_state &state, uint64_t id)
{
    for (uint64_t i = 0; i < state.pair_count; i++)
    {
        auto &pair = state.pairs[i];
        if (pair.allocated && pair.id == id)
            return &pair;
    }
    return nullptr;
}

uint64_t monotonic_millis()
{
    struct timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1'000'000;
}

bool next_read_deadline(const service_state &state, struct timespec &deadline)
{
    uint64_t nearest = 0;
    for (uint64_t i = 0; i < state.pending_read_count; i++)
    {
        const auto value = state.pending_reads[i].deadline_ms;
        if (value != 0 && (nearest == 0 || value < nearest))
            nearest = value;
    }
    if (nearest == 0)
        return false;
    deadline.tv_sec = static_cast<time_t>(nearest / 1000);
    deadline.tv_nsec = static_cast<long>((nearest % 1000) * 1'000'000);
    return true;
}

binding *find_binding(service_state &state, na_handle_t endpoint)
{
    for (uint64_t i = 0; i < state.binding_count; i++)
    {
        auto &item = state.bindings[i];
        if (item.endpoint == endpoint)
            return &item;
    }
    return nullptr;
}

terminal_open_description *find_open_description(service_state &state, uint64_t id)
{
    if (id == invalid_open_description || id >= max_endpoints || !state.open_descriptions[id].allocated)
        return nullptr;
    return &state.open_descriptions[id];
}

uint64_t allocate_open_description(service_state &state, uint64_t pair_id, bool master, uint64_t mode)
{
    for (uint64_t i = 0; i < max_endpoints; i++)
    {
        auto &description = state.open_descriptions[i];
        if (description.allocated)
            continue;
        description = {};
        description.allocated = true;
        description.pair_id = pair_id;
        description.master = master;
        description.status_flags = mode & terminal_status_nonblock;
        description.references = 1;
        return i;
    }
    return invalid_open_description;
}

void retain_open_description(service_state &state, uint64_t id)
{
    if (auto *description = find_open_description(state, id); description != nullptr)
        description->references++;
}

void release_open_description(service_state &state, uint64_t id)
{
    auto *description = find_open_description(state, id);
    if (description == nullptr)
        return;
    if (description->references > 1)
        description->references--;
    else
        *description = {};
}

void finalize_pair(service_state &state, pty_pair &pair)
{
    if (pair.id != 0 && state.free_pair_id_count < max_pairs)
        state.free_pair_ids[state.free_pair_id_count++] = pair.id;
    if (pair.job_control != NA_HANDLE_INVALID)
        (void)naos_handle_close(pair.job_control);
    if (pair.driver_control != NA_HANDLE_INVALID)
        (void)naos_handle_close(pair.driver_control);
    pair.job_control = NA_HANDLE_INVALID;
    pair.driver_control = NA_HANDLE_INVALID;
    pair.active_driver_invocation = NA_HANDLE_INVALID;
    pair.active_driver_sequence = 0;
    pair.next_driver_sequence = 1;
    pair.active_driver_responder = NA_HANDLE_INVALID;
    pair.pending_driver_action_count = 0;
    if (pair.pending_attribute_change.active)
        (void)naos_handle_close(pair.pending_attribute_change.responder);
    pair.pending_attribute_change = {};
    pair.allocated = false;
    pair.id = 0;
    pair.generation = 1;
    pair.factory_identity_id = 0;
    pair.factory_identity_generation = 0;
    pair.locator_token.fill(0);
    pair.slave_locked = true;
    pair.slave_granted = false;
    pair.slave_mode = 0620;
    pair.revoked = false;
    pair.retiring = false;
    pair.open_bindings = 0;
    pair.master_bindings = 0;
    pair.slave_bindings = 0;
}

bool valid_terminal_mode(uint64_t mode) { return (mode & ~7ULL) == 0 && (mode & 3ULL) != 0; }

void release_reserved_pair_id(service_state &state, uint64_t id)
{
    if (id != 0 && state.free_pair_id_count < max_pairs)
        state.free_pair_ids[state.free_pair_id_count++] = id;
}

bool valid_factory_result_resource(na_handle_t handle, uint64_t scope, const na_uuid_t &uuid, uint64_t revision,
                                   uint64_t features, uint64_t protocol_rights)
{
    if (handle == NA_HANDLE_INVALID)
        return false;
    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    if (_na_handle_get_info(handle, &info) != NA_STATUS_OK || info.binding != NA_BINDING_KERNEL_VIEW ||
        info.scope != scope || info.revision != revision || info.features != features ||
        std::memcmp(info.protocol_uuid.bytes, uuid.bytes, sizeof(uuid.bytes)) != 0 ||
        (info.meta_rights & (NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT)) !=
            (NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT) ||
        (info.protocol_rights & protocol_rights) != protocol_rights)
        return false;
    return true;
}

bool queue_pending_create(service_state &state, const naos::system::TerminalManager::create_pty_request &request,
                          na_handle_t responder, pending_create_kind kind = pending_create_kind::pty,
                          bool console_master = false)
{
    if (state.factory_handle == NA_HANDLE_INVALID || state.pending_create_count >= max_pairs)
        return false;

    uint64_t slot = max_pairs;
    for (uint64_t i = 0; i < max_pairs; i++)
        if (!state.pending_creates[i].active)
        {
            slot = i;
            break;
        }
    if (slot == max_pairs)
        return false;

    uint64_t id = 0;
    if (kind == pending_create_kind::console)
    {
        if (find_pair(state, 0) != nullptr)
            return false;
        for (uint64_t i = 0; i < max_pairs; i++)
            if (state.pending_creates[i].active && state.pending_creates[i].kind == pending_create_kind::console)
                return false;
    }
    else if (state.free_pair_id_count != 0)
        id = state.free_pair_ids[--state.free_pair_id_count];
    else
    {
        id = state.next_pair_id++;
        if (id == 0 || id > max_pairs)
            return false;
    }
    auto &pending = state.pending_creates[slot];
    pending = {};
    pending.pair_id = id;
    pending.kind = kind;
    pending.console_master = console_master;
    pending.generation = kind == pending_create_kind::console ? 1 : ++state.pair_generations[id];
    if (kind != pending_create_kind::console && pending.generation == 0)
        pending.generation = ++state.pair_generations[id];
    pending.wire = static_cast<std::uint8_t *>(malloc(NA_CHANNEL_MAX_MESSAGE_BYTES));
    if (pending.wire == nullptr)
    {
        release_reserved_pair_id(state, id);
        return false;
    }
    pending.request = request;
    pending.responder = responder;
    auto transport = make_transport();
    auto client =
        naos::system::TerminalDriverFactory::TerminalDriverFactoryClient(transport.async(), state.factory_handle);
    naos::system::TerminalDriverFactory::create_request factory_request{};
    factory_request.mode = request.mode;
    const auto status = client.submit_create(factory_request, nullptr, 0, &pending.invocation, pending.wire,
                                             NA_CHANNEL_MAX_MESSAGE_BYTES);
    if (status != NA_STATUS_OK)
    {
        free(pending.wire);
        pending = {};
        release_reserved_pair_id(state, id);
        return false;
    }
    pending.active = true;
    state.pending_create_count++;
    return true;
}

void finish_pending_create(service_state &state, pending_create &pending);

bool create_endpoint_pair(na_handle_t descriptor, na_handle_t &client, na_handle_t &server)
{
    const auto status = _na_protocol_endpoint_create(descriptor, nullptr, &client, &server);
    if (status != NA_STATUS_OK)
        _s_log("ttyd: endpoint create failed\n");
    return status == NA_STATUS_OK;
}

bool add_server_endpoint(service_state &state, na_handle_t server, uint64_t pair_id, bool master, uint64_t mode,
                         uint64_t open_description = invalid_open_description)
{
    if (state.endpoint_count >= max_endpoints || state.binding_count >= max_endpoints)
    {
        (void)naos_handle_close(server);
        return false;
    }
    if (open_description == invalid_open_description)
        open_description = allocate_open_description(state, pair_id, master, mode);
    else
    {
        const auto *description = find_open_description(state, open_description);
        if (description == nullptr || description->pair_id != pair_id || description->master != master)
        {
            (void)naos_handle_close(server);
            return false;
        }
        retain_open_description(state, open_description);
    }
    if (open_description == invalid_open_description)
    {
        (void)naos_handle_close(server);
        return false;
    }
    state.endpoints[state.endpoint_count++] = server;
    state.bindings[state.binding_count++] = {server, pair_id, master, mode, open_description};
    if (auto *pair = find_pair(state, pair_id))
    {
        pair->open_bindings++;
        if (master)
        {
            if (pair->master_bindings == 0)
                pair->core.open_master();
            pair->master_bindings++;
        }
        else
        {
            if (pair->slave_bindings == 0)
                pair->core.open_slave();
            pair->slave_bindings++;
        }
    }
    return true;
}

void remove_endpoint(service_state &state, na_handle_t endpoint)
{
    uint64_t removed_pair_id = 0;
    uint64_t removed_open_description = invalid_open_description;
    bool removed_master = false;
    bool removed_binding = false;
    for (uint64_t i = 0; i < state.endpoint_count; i++)
    {
        if (state.endpoints[i] == endpoint)
        {
            for (uint64_t j = i + 1; j < state.endpoint_count; j++)
                state.endpoints[j - 1] = state.endpoints[j];
            state.endpoint_count--;
            break;
        }
    }
    for (uint64_t i = 0; i < state.binding_count; i++)
    {
        if (state.bindings[i].endpoint == endpoint)
        {
            removed_pair_id = state.bindings[i].pair_id;
            removed_master = state.bindings[i].master;
            removed_open_description = state.bindings[i].open_description;
            removed_binding = true;
            for (uint64_t j = i + 1; j < state.binding_count; j++)
                state.bindings[j - 1] = state.bindings[j];
            state.binding_count--;
            break;
        }
    }
    if (!removed_binding)
        return;
    release_open_description(state, removed_open_description);
    for (uint64_t i = 0; i < state.pair_count; i++)
    {
        auto &pair = state.pairs[i];
        if (!pair.allocated || pair.id != removed_pair_id)
            continue;
        if (pair.open_bindings != 0)
            pair.open_bindings--;
        if (pair.pending_attribute_change.active && pair.pending_attribute_change.endpoint == endpoint)
        {
            (void)naos_handle_close(pair.pending_attribute_change.responder);
            pair.pending_attribute_change = {};
        }
        if (removed_master)
        {
            if (pair.master_bindings != 0)
                pair.master_bindings--;
            if (pair.master_bindings == 0)
            {
                pair.core.hangup_master();
                pair.revoked = true;
                if (!queue_driver_action(pair, driver_action_kind::hangup, 0))
                    _s_log("ttyd: unable to queue driver hangup\n");
            }
        }
        else
        {
            if (pair.slave_bindings != 0)
                pair.slave_bindings--;
            // The physical console has a persistent master owned by
            // consoled. A shell may close and reopen /dev/console during a
            // restart; that must not hang up the console master. Ordinary
            // PTYs retain the POSIX last-slave hangup behavior.
            if (pair.slave_bindings == 0 && pair.id != 0)
            {
                pair.core.hangup_slave();
            }
        }
        if (pair.open_bindings == 0)
        {
            if (pair.active_driver_invocation != NA_HANDLE_INVALID || pair.pending_driver_action_count != 0)
                pair.retiring = true;
            else
                finalize_pair(state, pair);
        }
        break;
    }
    flush_pending_reads(state);
    drop_pending_writes(state, endpoint);
    flush_pending_watches(state);
}

void rollback_server_endpoint(service_state &state, na_handle_t endpoint)
{
    if (endpoint == NA_HANDLE_INVALID)
        return;
    (void)naos_handle_close(endpoint);
    remove_endpoint(state, endpoint);
}

void finish_pending_create(service_state &state, pending_create &pending)
{
    if (!pending.active)
        return;

    const auto responder = pending.responder;
    const auto pair_id = pending.pair_id;
    bool pair_published = false;
    auto discard = [&](na_outcome_reason_t reason, bool notify) {
        if (notify && responder != NA_HANDLE_INVALID)
            reject_responder(responder, reason);
        if (responder != NA_HANDLE_INVALID)
            (void)naos_handle_close(responder);
        if (pending.invocation != NA_HANDLE_INVALID)
        {
            (void)_na_invocation_cancel(pending.invocation);
            (void)naos_handle_close(pending.invocation);
        }
        if (pending.wire != nullptr)
            free(pending.wire);
        if (!pair_published)
            release_reserved_pair_id(state, pair_id);
        pending = {};
        if (state.pending_create_count != 0)
            state.pending_create_count--;
    };

    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    if (responder_cancelled_or_closed(responder))
    {
        discard(NA_OUTCOME_REASON_CANCEL_REQUESTED, false);
        return;
    }
    if (_na_handle_get_info(pending.invocation, &info) != NA_STATUS_OK ||
        (info.signals & (NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED)) == 0)
        return;

    auto transport = make_transport();
    auto client =
        naos::system::TerminalDriverFactory::TerminalDriverFactoryClient(transport.async(), state.factory_handle);
    naos::system::TerminalDriverFactory::create_response factory_response{};
    na_handle_t resources[NA_CHANNEL_MAX_RESOURCES]{};
    na_result_frame_t result{};
    const auto status = client.take_create(pending.invocation, factory_response, pending.wire,
                                           NA_CHANNEL_MAX_MESSAGE_BYTES, resources, NA_CHANNEL_MAX_RESOURCES, result);
    const auto close_factory_resources = [&]() {
        const auto count =
            result.actual_resources > NA_CHANNEL_MAX_RESOURCES ? NA_CHANNEL_MAX_RESOURCES : result.actual_resources;
        for (uint64_t i = 0; i < count; i++)
            if (resources[i] != NA_HANDLE_INVALID)
                (void)naos_handle_close(resources[i]);
    };
    if (status != NA_STATUS_OK)
    {
        discard(NA_OUTCOME_REASON_BROKER_FAILURE, true);
        return;
    }
    const int factory_error = naos::result_errno(result);
    if (factory_error != 0 || result.actual_resources != 2 ||
        factory_response.driver.value >= result.actual_resources ||
        factory_response.job_control.value >= result.actual_resources ||
        factory_response.driver.value == factory_response.job_control.value ||
        !valid_factory_result_resource(resources[factory_response.driver.value], NA_SCOPE_TERMINAL_DRIVER_CONTROL,
                                       naos::system::TerminalDriverControl::protocol_uuid,
                                       naos::system::TerminalDriverControl::revision,
                                       naos::system::TerminalDriverControl::features, NA_PROTOCOL_RIGHT_INVOKE) ||
        !valid_factory_result_resource(resources[factory_response.job_control.value], NA_SCOPE_TERMINAL_JOB_CONTROL,
                                       naos::system::TerminalJobControl::protocol_uuid,
                                       naos::system::TerminalJobControl::revision,
                                       naos::system::TerminalJobControl::features, NA_PROTOCOL_RIGHT_INVOKE))
    {
        close_factory_resources();
        discard(factory_error == EAGAIN ? NA_OUTCOME_REASON_REQUEST_DISCARDED : NA_OUTCOME_REASON_PROTOCOL_VIOLATION,
                true);
        return;
    }

    const auto driver_control = resources[factory_response.driver.value];
    const auto job_control = resources[factory_response.job_control.value];
    resources[factory_response.driver.value] = NA_HANDLE_INVALID;
    resources[factory_response.job_control.value] = NA_HANDLE_INVALID;
    close_factory_resources();

    if (pending.kind == pending_create_kind::console)
    {
        if (find_pair(state, 0) != nullptr)
        {
            (void)naos_handle_close(driver_control);
            (void)naos_handle_close(job_control);
            discard(NA_OUTCOME_REASON_REQUEST_DISCARDED, true);
            return;
        }

        uint64_t slot_index = state.pair_count;
        for (uint64_t i = 0; i < state.pair_count; i++)
            if (!state.pairs[i].allocated)
            {
                slot_index = i;
                break;
            }
        if (slot_index == state.pair_count && state.pair_count >= max_pairs)
        {
            (void)naos_handle_close(driver_control);
            (void)naos_handle_close(job_control);
            discard(NA_OUTCOME_REASON_REQUEST_DISCARDED, true);
            return;
        }

        na_handle_t client_endpoint = NA_HANDLE_INVALID;
        na_handle_t server_endpoint = NA_HANDLE_INVALID;
        const auto descriptor = pending.console_master ? state.master_descriptor : state.slave_descriptor;
        if (!create_endpoint_pair(descriptor, client_endpoint, server_endpoint))
        {
            (void)naos_handle_close(driver_control);
            (void)naos_handle_close(job_control);
            discard(NA_OUTCOME_REASON_BROKER_FAILURE, true);
            return;
        }

        pty_pair staged{};
        staged.allocated = true;
        staged.id = 0;
        staged.generation = 1;
        staged.slave_locked = false;
        staged.slave_granted = true;
        staged.job_control = job_control;
        staged.driver_control = driver_control;
        staged.factory_identity_id = factory_response.locator.terminal_id;
        staged.factory_identity_generation = factory_response.locator.generation;
        staged.locator_token = factory_response.locator.token;
        if (slot_index == state.pair_count)
            state.pair_count++;
        state.pairs[slot_index] = std::move(staged);
        pair_published = true;
        auto &pair = state.pairs[slot_index];
        pair.core.set_control_event_handler(ttyd_control_event, &pair);
        if (!add_server_endpoint(state, server_endpoint, pair.id, pending.console_master, pending.request.mode))
        {
            (void)naos_handle_close(client_endpoint);
            finalize_pair(state, pair);
            discard(NA_OUTCOME_REASON_REQUEST_DISCARDED, true);
            return;
        }

        client_endpoint = restrict_terminal_client(client_endpoint, pending.request.mode);
        na_handle_t public_job_control = public_job_control_handle(pair.job_control);
        std::uint8_t response_wire[512]{};
        uint64_t response_bytes = 0;
        bool encoded = false;
        if (pending.console_master)
        {
            naos::system::TerminalManager::open_console_master_response response{};
            response.master.value = 0;
            response.job_control.value = 1;
            encoded = public_job_control != NA_HANDLE_INVALID &&
                      naos::system::TerminalManager::encode_open_console_master_response(
                          response_wire, sizeof(response_wire), response, response_bytes);
        }
        else
        {
            naos::system::TerminalManager::open_console_response response{};
            response.slave.value = 0;
            response.job_control.value = 1;
            encoded = public_job_control != NA_HANDLE_INVALID &&
                      naos::system::TerminalManager::encode_open_console_response(response_wire, sizeof(response_wire),
                                                                                  response, response_bytes);
        }
        if (!encoded ||
            !validate_response_resource(client_endpoint,
                                        pending.console_master ? NA_SCOPE_TERMINAL_MASTER : NA_SCOPE_TERMINAL_SLAVE,
                                        pending.console_master ? naos::system::TerminalMaster::protocol_uuid
                                                               : naos::system::TerminalSlave::protocol_uuid,
                                        NA_BINDING_CLIENT_END) ||
            !validate_response_resource(public_job_control, NA_SCOPE_TERMINAL_JOB_CONTROL,
                                        naos::system::TerminalJobControl::protocol_uuid, NA_BINDING_KERNEL_VIEW))
        {
            (void)naos_handle_close(client_endpoint);
            if (public_job_control != NA_HANDLE_INVALID)
                (void)naos_handle_close(public_job_control);
            rollback_server_endpoint(state, server_endpoint);
            discard(NA_OUTCOME_REASON_PROTOCOL_VIOLATION, true);
            return;
        }

        na_resource_disposition_t response_resources[2]{};
        response_resources[0] = {client_endpoint, NA_RESOURCE_MOVE, 0,
                                 NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                                 pending.console_master ? NA_SCOPE_TERMINAL_MASTER : NA_SCOPE_TERMINAL_SLAVE};
        response_resources[1] = {public_job_control, NA_RESOURCE_MOVE, 0,
                                 NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT, NA_SCOPE_TERMINAL_JOB_CONTROL};
        na_reply_frame_t reply{};
        reply.struct_size = sizeof(reply);
        reply.bytes = reinterpret_cast<std::uint64_t>(response_wire);
        reply.byte_count = response_bytes;
        reply.resources = reinterpret_cast<std::uint64_t>(response_resources);
        reply.resource_count = 2;
        const auto reply_status = _na_responder_reply(responder, &reply);
        if (reply_status != NA_STATUS_OK)
        {
            (void)naos_handle_close(client_endpoint);
            (void)naos_handle_close(public_job_control);
            rollback_server_endpoint(state, server_endpoint);
            discard(NA_OUTCOME_REASON_PEER_CLOSED, false);
            return;
        }
        (void)naos_handle_close(pending.invocation);
        free(pending.wire);
        pending = {};
        if (state.pending_create_count != 0)
            state.pending_create_count--;
        return;
    }

    uint64_t slot_index = state.pair_count;
    for (uint64_t i = 0; i < state.pair_count; i++)
        if (!state.pairs[i].allocated)
        {
            slot_index = i;
            break;
        }
    if (slot_index == state.pair_count && state.pair_count >= max_pairs)
    {
        (void)naos_handle_close(driver_control);
        (void)naos_handle_close(job_control);
        discard(NA_OUTCOME_REASON_REQUEST_DISCARDED, true);
        return;
    }

    na_handle_t client_endpoint = NA_HANDLE_INVALID;
    na_handle_t server_endpoint = NA_HANDLE_INVALID;
    if (!create_endpoint_pair(state.master_descriptor, client_endpoint, server_endpoint))
    {
        (void)naos_handle_close(driver_control);
        (void)naos_handle_close(job_control);
        discard(NA_OUTCOME_REASON_BROKER_FAILURE, true);
        return;
    }

    pty_pair staged{};
    staged.allocated = true;
    staged.id = pair_id;
    staged.generation = pending.generation;
    staged.slave_locked = pending.request.locked != 0;
    staged.slave_granted = false;
    staged.job_control = job_control;
    staged.driver_control = driver_control;
    staged.factory_identity_id = factory_response.locator.terminal_id;
    staged.factory_identity_generation = factory_response.locator.generation;
    staged.locator_token = factory_response.locator.token;
    (void)staged.core.set_termios(to_ttyd_termios(pending.request.attributes));
    staged.core.set_winsize(to_ttyd_winsize(pending.request.size));
    if (slot_index == state.pair_count)
        state.pair_count++;
    state.pairs[slot_index] = std::move(staged);
    pair_published = true;
    auto &pair = state.pairs[slot_index];
    pair.core.set_control_event_handler(ttyd_control_event, &pair);
    if (!add_server_endpoint(state, server_endpoint, pair_id, true, pending.request.mode))
    {
        (void)naos_handle_close(client_endpoint);
        finalize_pair(state, pair);
        discard(NA_OUTCOME_REASON_REQUEST_DISCARDED, true);
        return;
    }

    client_endpoint = restrict_terminal_client(client_endpoint, pending.request.mode);
    naos::system::TerminalManager::create_pty_response response{};
    response.master.value = 0;
    response.job_control.value = 1;
    response.slave_locator.pair_id = pair_id;
    response.slave_locator.generation = pair.generation;
    response.slave_locator.token = pair.locator_token;
    response.number = pair_id;
    na_handle_t public_job_control = public_job_control_handle(pair.job_control);
    std::uint8_t response_wire[512]{};
    uint64_t response_bytes = 0;
    if (public_job_control == NA_HANDLE_INVALID || !naos::system::TerminalManager::encode_create_pty_response(
                                                       response_wire, sizeof(response_wire), response, response_bytes))
    {
        if (public_job_control != NA_HANDLE_INVALID)
            (void)naos_handle_close(public_job_control);
        rollback_server_endpoint(state, server_endpoint);
        discard(NA_OUTCOME_REASON_BROKER_FAILURE, true);
        return;
    }
    na_resource_disposition_t response_resources[2]{};
    response_resources[0] = {client_endpoint, NA_RESOURCE_MOVE, 0, NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                             NA_SCOPE_TERMINAL_MASTER};
    response_resources[1] = {public_job_control, NA_RESOURCE_MOVE, 0,
                             NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT, NA_SCOPE_TERMINAL_JOB_CONTROL};
    na_reply_frame_t reply{};
    reply.struct_size = sizeof(reply);
    reply.bytes = reinterpret_cast<std::uint64_t>(response_wire);
    reply.byte_count = response_bytes;
    reply.resources = reinterpret_cast<std::uint64_t>(response_resources);
    reply.resource_count = 2;
    const auto reply_status = _na_responder_reply(responder, &reply);
    if (reply_status != NA_STATUS_OK)
    {
        (void)naos_handle_close(client_endpoint);
        (void)naos_handle_close(public_job_control);
        rollback_server_endpoint(state, server_endpoint);
    }
    (void)naos_handle_close(pending.invocation);
    free(pending.wire);
    pending = {};
    if (state.pending_create_count != 0)
        state.pending_create_count--;
}

void flush_pending_creates(service_state &state)
{
    for (uint64_t i = 0; i < max_pairs; i++)
        if (state.pending_creates[i].active)
            finish_pending_create(state, state.pending_creates[i]);
}

void drop_pending_reads(service_state &state, na_handle_t endpoint)
{
    uint64_t i = 0;
    while (i < state.pending_read_count)
    {
        auto &pending = state.pending_reads[i];
        if (pending.endpoint != endpoint)
        {
            i++;
            continue;
        }
        (void)naos_handle_close(pending.responder);
        pending = state.pending_reads[--state.pending_read_count];
    }
}

void drop_pending_watches(service_state &state, na_handle_t endpoint)
{
    uint64_t i = 0;
    while (i < state.pending_watch_count)
    {
        auto &pending = state.pending_watches[i];
        if (pending.endpoint != endpoint)
        {
            i++;
            continue;
        }
        (void)naos_handle_close(pending.responder);
        pending = state.pending_watches[--state.pending_watch_count];
    }
}

bool add_pending_write(service_state &state, na_handle_t endpoint, na_handle_t responder, std::uint8_t *data,
                       std::size_t size, std::size_t offset, bool master, bool nonblock = false)
{
    if (data == nullptr || offset > size || state.pending_write_count >= max_endpoints)
    {
        if (data != nullptr)
            free(data);
        state.quota_rejections++;
        return false;
    }
    std::uint64_t per_binding = 0;
    for (uint64_t i = 0; i < state.pending_write_count; i++)
        per_binding += state.pending_writes[i].endpoint == endpoint;
    if (per_binding >= max_pending_per_binding)
    {
        free(data);
        state.quota_rejections++;
        return false;
    }
    const auto sequence = state.next_write_sequence++;
    if (state.next_write_sequence == 0)
        state.next_write_sequence = 1;
    state.pending_writes[state.pending_write_count++] = {endpoint, responder, data,     size,
                                                         offset,   master,    sequence, nonblock};
    return true;
}

void drop_pending_writes(service_state &state, na_handle_t endpoint)
{
    uint64_t i = 0;
    while (i < state.pending_write_count)
    {
        auto &pending = state.pending_writes[i];
        if (pending.endpoint != endpoint)
        {
            i++;
            continue;
        }
        free(pending.data);
        (void)naos_handle_close(pending.responder);
        pending = state.pending_writes[--state.pending_write_count];
    }
}

void finish_pending_write(pending_write &pending, std::uint64_t count)
{
    std::uint8_t wire[256]{};
    std::uint64_t written = 0;
    bool encoded = false;
    if (pending.master)
    {
        naos::system::TerminalMaster::write_response response{};
        response.count = count;
        encoded = naos::system::TerminalMaster::encode_write_response(wire, sizeof(wire), response, written);
    }
    else
    {
        naos::system::TerminalSlave::write_response response{};
        response.count = count;
        encoded = naos::system::TerminalSlave::encode_write_response(wire, sizeof(wire), response, written);
    }
    na_reply_frame_t reply{};
    reply.struct_size = sizeof(reply);
    reply.bytes = written == 0 ? 0 : reinterpret_cast<std::uint64_t>(wire);
    reply.byte_count = written;
    const auto reply_status = encoded ? _na_responder_reply(pending.responder, &reply) : NA_STATUS_INVALID_MESSAGE;
    if (reply_status != NA_STATUS_OK)
        (void)naos_handle_close(pending.responder);
}

void flush_pending_writes(service_state &state)
{
    uint64_t i = 0;
    while (i < state.pending_write_count)
    {
        auto &pending = state.pending_writes[i];
        if (responder_cancelled_or_closed(pending.responder))
        {
            free(pending.data);
            (void)naos_handle_close(pending.responder);
            pending = state.pending_writes[--state.pending_write_count];
            continue;
        }
        auto *binding = find_binding(state, pending.endpoint);
        auto *pair = binding == nullptr ? nullptr : find_pair(state, binding->pair_id);
        if (pair == nullptr)
        {
            free(pending.data);
            reject_responder(pending.responder, NA_OUTCOME_REASON_OBJECT_REVOKED);
            pending = state.pending_writes[--state.pending_write_count];
            continue;
        }
        bool is_oldest = true;
        for (uint64_t candidate = 0; candidate < state.pending_write_count; candidate++)
        {
            const auto &other = state.pending_writes[candidate];
            if (other.endpoint == pending.endpoint && other.sequence < pending.sequence)
            {
                is_oldest = false;
                break;
            }
        }
        if (!is_oldest)
        {
            i++;
            continue;
        }
        const std::size_t remaining = pending.size - pending.offset;
        std::size_t progressed = 0;
        const int result = pending.master
                               ? pair->core.receive_input(pending.data + pending.offset, remaining, true, &progressed)
                               : pair->core.write_output(pending.data + pending.offset, remaining, true, &progressed);
        if (!pending.master)
        {
            char message[96]{};
            snprintf(message, sizeof(message), "ttyd: slave write size=%llu offset=%llu progressed=%llu result=%d\n",
                     static_cast<unsigned long long>(pending.size), static_cast<unsigned long long>(pending.offset),
                     static_cast<unsigned long long>(progressed), result);
            _s_log(message);
        }
        pending.offset += progressed;
        if (pending.nonblock)
        {
            if (result < 0 && (result != -EAGAIN || progressed == 0))
            {
                free(pending.data);
                reject_responder(pending.responder, terminal_error_reason(result));
                pending = state.pending_writes[--state.pending_write_count];
                continue;
            }
            finish_pending_write(pending, pending.offset);
            free(pending.data);
            pending = state.pending_writes[--state.pending_write_count];
            continue;
        }
        if (pending.offset == pending.size)
        {
            finish_pending_write(pending, pending.size);
            free(pending.data);
            pending = state.pending_writes[--state.pending_write_count];
            continue;
        }
        if (result < 0 && result != -EAGAIN)
        {
            if (pending.offset != 0)
            {
                finish_pending_write(pending, pending.offset);
                free(pending.data);
                pending = state.pending_writes[--state.pending_write_count];
                continue;
            }
            free(pending.data);
            reject_responder(pending.responder, terminal_error_reason(result));
            pending = state.pending_writes[--state.pending_write_count];
            continue;
        }
        i++;
    }
}

void flush_pending_attributes(service_state &state)
{
    for (uint64_t i = 0; i < state.pair_count; i++)
    {
        auto &pair = state.pairs[i];
        auto &pending = pair.pending_attribute_change;
        if (!pair.allocated || !pending.active)
            continue;
        if (responder_cancelled_or_closed(pending.responder))
        {
            (void)naos_handle_close(pending.responder);
            pending = {};
            continue;
        }
        if (pair.core.output_available() != 0)
            continue;
        if (pending.flush_input)
            (void)pair.core.flush(ttyd::tty_flush::input);
        (void)pair.core.set_termios(pending.attributes);
        na_reply_frame_t reply{};
        reply.struct_size = sizeof(reply);
        if (_na_responder_reply(pending.responder, &reply) != NA_STATUS_OK)
            (void)naos_handle_close(pending.responder);
        pending = {};
    }
}

bool endpoint_has_pending_write(const service_state &state, na_handle_t endpoint)
{
    for (uint64_t i = 0; i < state.pending_write_count; i++)
        if (state.pending_writes[i].endpoint == endpoint)
            return true;
    return false;
}

bool is_pending_responder(const service_state &state, na_handle_t handle)
{
    if (handle == NA_HANDLE_INVALID)
        return false;
    for (uint64_t i = 0; i < state.pending_read_count; i++)
        if (state.pending_reads[i].responder == handle)
            return true;
    for (uint64_t i = 0; i < state.pending_write_count; i++)
        if (state.pending_writes[i].responder == handle)
            return true;
    for (uint64_t i = 0; i < state.pending_watch_count; i++)
        if (state.pending_watches[i].responder == handle)
            return true;
    for (uint64_t i = 0; i < max_pairs; i++)
    {
        const auto &create = state.pending_creates[i];
        if (create.active && create.responder == handle)
            return true;
        const auto &locator = state.pending_locator_opens[i];
        if (locator.active && locator.responder == handle)
            return true;
    }
    for (uint64_t i = 0; i < state.pair_count; i++)
    {
        const auto &pair = state.pairs[i];
        if (!pair.allocated)
            continue;
        if (pair.pending_attribute_change.active && pair.pending_attribute_change.responder == handle)
            return true;
        if (pair.active_driver_responder == handle)
            return true;
        for (uint64_t j = 0; j < pair.pending_driver_action_count; j++)
            if (pair.pending_driver_actions[j].responder == handle)
                return true;
    }
    return false;
}

bool validate_response_resource(na_handle_t handle, uint64_t scope, const na_uuid_t &protocol_uuid, uint32_t binding)
{
    if (handle == NA_HANDLE_INVALID)
        return false;
    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    const auto expected_revision = scope == NA_SCOPE_TERMINAL_MASTER  ? naos::system::TerminalMaster::revision
                                   : scope == NA_SCOPE_TERMINAL_SLAVE ? naos::system::TerminalSlave::revision
                                                                      : naos::system::TerminalJobControl::revision;
    const bool valid = _na_handle_get_info(handle, &info) == NA_STATUS_OK && info.binding == binding &&
                       info.scope == scope &&
                       std::memcmp(info.protocol_uuid.bytes, protocol_uuid.bytes, sizeof(protocol_uuid.bytes)) == 0 &&
                       info.revision == expected_revision &&
                       (info.meta_rights & (NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT)) ==
                           (NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT) &&
                       (info.protocol_rights & NA_PROTOCOL_RIGHT_INVOKE) != 0;
    if (!valid)
        return false;
    return true;
}

uint64_t terminal_client_rights(uint64_t mode)
{
    uint64_t rights =
        NA_PROTOCOL_RIGHT_INVOKE | NA_TERMINAL_RIGHT_CONTROL | NA_TERMINAL_RIGHT_WATCH | NA_TERMINAL_RIGHT_ADMIN;
    if ((mode & 1) != 0)
        rights |= NA_TERMINAL_RIGHT_READ;
    if ((mode & 2) != 0)
        rights |= NA_TERMINAL_RIGHT_WRITE;
    return rights;
}

na_handle_t restrict_terminal_client(na_handle_t client, uint64_t mode)
{
    if (client == NA_HANDLE_INVALID)
        return NA_HANDLE_INVALID;
    na_handle_restriction_t restriction{};
    restriction.struct_size = sizeof(restriction);
    restriction.flags = NA_RESTRICTION_PROTOCOL_RIGHTS;
    restriction.protocol_rights = terminal_client_rights(mode);
    na_handle_t restricted = NA_HANDLE_INVALID;
    if (_na_handle_restrict(client, &restriction, &restricted) != NA_STATUS_OK)
    {
        (void)naos_handle_close(client);
        return NA_HANDLE_INVALID;
    }
    (void)naos_handle_close(client);
    return restricted;
}

bool set_response_resource(request_context &context, na_handle_t client, uint64_t scope, const na_uuid_t &protocol_uuid,
                           uint32_t binding = NA_BINDING_CLIENT_END, uint64_t terminal_mode = 3)
{
    if (context.response_resources == nullptr || context.response_resource_count == 0)
    {
        (void)naos_handle_close(client);
        context.outcome = naoidl::dispatch_outcome::failed;
        context.failure_reason = NA_OUTCOME_REASON_BROKER_FAILURE;
        return false;
    }
    if (scope == NA_SCOPE_TERMINAL_MASTER || scope == NA_SCOPE_TERMINAL_SLAVE)
        client = restrict_terminal_client(client, terminal_mode);
    if (!validate_response_resource(client, scope, protocol_uuid, binding))
    {
        (void)naos_handle_close(client);
        context.outcome = naoidl::dispatch_outcome::failed;
        context.failure_reason = NA_OUTCOME_REASON_PROTOCOL_VIOLATION;
        return false;
    }
    context.response_resources[0] = {};
    context.response_resources[0].handle = client;
    context.response_resources[0].operation = NA_RESOURCE_MOVE;
    context.response_resources[0].rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    context.response_resources[0].scope = scope;
    return true;
}

bool set_two_response_resources(request_context &context, na_handle_t primary, na_handle_t secondary,
                                uint64_t primary_scope, uint64_t terminal_mode = 3)
{
    if (context.response_resources == nullptr || context.response_resource_count < 2)
    {
        (void)naos_handle_close(primary);
        (void)naos_handle_close(secondary);
        context.outcome = naoidl::dispatch_outcome::failed;
        context.failure_reason = NA_OUTCOME_REASON_BROKER_FAILURE;
        return false;
    }
    primary = restrict_terminal_client(primary, terminal_mode);
    if (!validate_response_resource(primary, primary_scope,
                                    primary_scope == NA_SCOPE_TERMINAL_MASTER
                                        ? naos::system::TerminalMaster::protocol_uuid
                                        : naos::system::TerminalSlave::protocol_uuid,
                                    NA_BINDING_CLIENT_END) ||
        !validate_response_resource(secondary, NA_SCOPE_TERMINAL_JOB_CONTROL,
                                    naos::system::TerminalJobControl::protocol_uuid, NA_BINDING_KERNEL_VIEW))
    {
        (void)naos_handle_close(primary);
        (void)naos_handle_close(secondary);
        context.outcome = naoidl::dispatch_outcome::failed;
        context.failure_reason = NA_OUTCOME_REASON_PROTOCOL_VIOLATION;
        return false;
    }
    context.response_resources[0] = {};
    context.response_resources[0].handle = primary;
    context.response_resources[0].operation = NA_RESOURCE_MOVE;
    context.response_resources[0].rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    context.response_resources[0].scope = primary_scope;
    context.response_resources[1] = {};
    context.response_resources[1].handle = secondary;
    context.response_resources[1].operation = NA_RESOURCE_MOVE;
    context.response_resources[1].rights = NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT;
    context.response_resources[1].scope = NA_SCOPE_TERMINAL_JOB_CONTROL;
    return true;
}

na_handle_t duplicate_handle(na_handle_t source)
{
    if (source == NA_HANDLE_INVALID)
        return NA_HANDLE_INVALID;
    na_handle_t result = NA_HANDLE_INVALID;
    const auto status = _na_handle_duplicate(source, 0, &result);
    return status == NA_STATUS_OK ? result : NA_HANDLE_INVALID;
}

na_handle_t public_job_control_handle(na_handle_t source)
{
    const auto duplicate = duplicate_handle(source);
    if (duplicate == NA_HANDLE_INVALID)
        return NA_HANDLE_INVALID;
    na_handle_restriction_t restriction{};
    restriction.struct_size = sizeof(restriction);
    restriction.flags = NA_RESTRICTION_PROTOCOL_RIGHTS;
    restriction.protocol_rights = NA_PROTOCOL_RIGHT_INVOKE;
    na_handle_t result = NA_HANDLE_INVALID;
    if (_na_handle_restrict(duplicate, &restriction, &result) != NA_STATUS_OK)
    {
        (void)naos_handle_close(duplicate);
        return NA_HANDLE_INVALID;
    }
    (void)naos_handle_close(duplicate);
    return result;
}

bool queue_locator_validation(service_state &state, pending_locator_kind kind, uint64_t pair_id, uint64_t generation,
                              const std::array<std::uint8_t, 16> &token, uint64_t mode, uint64_t caller_pid,
                              na_handle_t responder)
{
    if (state.factory_handle == NA_HANDLE_INVALID || state.pending_locator_open_count >= max_pairs || pair_id == 0 ||
        generation == 0 || caller_pid == 0)
        return false;

    uint64_t slot = max_pairs;
    for (uint64_t i = 0; i < max_pairs; i++)
        if (!state.pending_locator_opens[i].active)
        {
            slot = i;
            break;
        }
    if (slot == max_pairs)
        return false;

    auto &pending = state.pending_locator_opens[slot];
    pending = {};
    pending.kind = kind;
    pending.pair_id = pair_id;
    pending.generation = generation;
    pending.token = token;
    pending.mode = mode;
    pending.caller_pid = caller_pid;
    pending.responder = responder;
    pending.wire = static_cast<std::uint8_t *>(malloc(256));
    if (pending.wire == nullptr)
        return false;

    auto transport = make_transport();
    auto client =
        naos::system::TerminalDriverFactory::TerminalDriverFactoryClient(transport.async(), state.factory_handle);
    naos::system::TerminalDriverFactory::validate_locator_request request{};
    auto *pair = find_pair(state, pair_id);
    if (pair == nullptr)
    {
        free(pending.wire);
        pending = {};
        return false;
    }
    request.locator.terminal_id = pair->factory_identity_id;
    request.locator.generation = pair->factory_identity_generation;
    request.locator.token = pair->locator_token;
    const auto submit = client.submit_validate_locator(request, nullptr, 0, &pending.invocation, pending.wire, 256);
    if (submit != NA_STATUS_OK)
    {
        free(pending.wire);
        pending = {};
        return false;
    }
    pending.active = true;
    state.pending_locator_open_count++;
    return true;
}

void finish_pending_locator_open(service_state &state, pending_locator_open &pending)
{
    if (!pending.active)
        return;

    const auto responder = pending.responder;
    auto discard = [&](na_outcome_reason_t reason, bool notify) {
        if (notify && responder != NA_HANDLE_INVALID)
            reject_responder(responder, reason);
        if (responder != NA_HANDLE_INVALID)
            (void)naos_handle_close(responder);
        if (pending.invocation != NA_HANDLE_INVALID)
        {
            (void)_na_invocation_cancel(pending.invocation);
            (void)naos_handle_close(pending.invocation);
        }
        free(pending.wire);
        pending = {};
        if (state.pending_locator_open_count != 0)
            state.pending_locator_open_count--;
    };

    if (responder_cancelled_or_closed(responder))
    {
        discard(NA_OUTCOME_REASON_CANCEL_REQUESTED, false);
        return;
    }
    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    if (_na_handle_get_info(pending.invocation, &info) != NA_STATUS_OK ||
        (info.signals & (NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED)) == 0)
        return;

    auto transport = make_transport();
    auto client =
        naos::system::TerminalDriverFactory::TerminalDriverFactoryClient(transport.async(), state.factory_handle);
    naos::system::TerminalDriverFactory::validate_locator_response response{};
    na_result_frame_t result{};
    const auto status =
        client.take_validate_locator(pending.invocation, response, pending.wire, 256, nullptr, 0, result);
    if (status != NA_STATUS_OK || naos::result_errno(result) != 0 || response.valid == 0)
    {
        discard(status == NA_STATUS_OK ? NA_OUTCOME_REASON_REQUEST_DISCARDED : NA_OUTCOME_REASON_BROKER_FAILURE, true);
        return;
    }

    auto *pair = find_pair(state, pending.pair_id);
    // The opaque locator is the authorization for a PTY slave. It survives
    // fork with the master fd; a PID is neither stable across fork nor an
    // authority boundary. Controlling-terminal locators are authorized by
    // the kernel session lookup instead.
    const bool locator_authorized = pending.kind == pending_locator_kind::controlling ||
                                    pending.kind == pending_locator_kind::slave_locator ||
                                    pending.kind == pending_locator_kind::slave_by_number;
    const bool locator_matches = pair != nullptr && pair->allocated && !pair->revoked && !pair->retiring &&
                                 pair->generation == pending.generation && pair->locator_token == pending.token &&
                                 locator_authorized && !pair->slave_locked && pair->slave_granted;
    if (!locator_matches || state.endpoint_count >= max_endpoints || state.binding_count >= max_endpoints)
    {
        discard(NA_OUTCOME_REASON_REQUEST_DISCARDED, true);
        return;
    }

    na_handle_t client_endpoint = NA_HANDLE_INVALID;
    na_handle_t server_endpoint = NA_HANDLE_INVALID;
    if (!create_endpoint_pair(state.slave_descriptor, client_endpoint, server_endpoint))
    {
        discard(NA_OUTCOME_REASON_BROKER_FAILURE, true);
        return;
    }
    if (!add_server_endpoint(state, server_endpoint, pair->id, false, pending.mode))
    {
        (void)naos_handle_close(client_endpoint);
        discard(NA_OUTCOME_REASON_REQUEST_DISCARDED, true);
        return;
    }
    client_endpoint = restrict_terminal_client(client_endpoint, pending.mode);
    const auto public_job_control = public_job_control_handle(pair->job_control);
    std::uint8_t response_wire[512]{};
    uint64_t response_bytes = 0;
    bool encoded = false;
    if (pending.kind == pending_locator_kind::slave_locator)
    {
        naos::system::TerminalManager::open_pty_slave_response typed_response{};
        typed_response.slave.value = 0;
        typed_response.job_control.value = 1;
        encoded = public_job_control != NA_HANDLE_INVALID &&
                  naos::system::TerminalManager::encode_open_pty_slave_response(response_wire, sizeof(response_wire),
                                                                                typed_response, response_bytes);
    }
    else if (pending.kind == pending_locator_kind::slave_by_number)
    {
        naos::system::TerminalManager::open_pty_slave_by_number_response typed_response{};
        typed_response.slave.value = 0;
        typed_response.job_control.value = 1;
        encoded = public_job_control != NA_HANDLE_INVALID &&
                  naos::system::TerminalManager::encode_open_pty_slave_by_number_response(
                      response_wire, sizeof(response_wire), typed_response, response_bytes);
    }
    else
    {
        naos::system::TerminalManager::open_controlling_response typed_response{};
        typed_response.slave.value = 0;
        typed_response.job_control.value = 1;
        encoded = public_job_control != NA_HANDLE_INVALID &&
                  naos::system::TerminalManager::encode_open_controlling_response(response_wire, sizeof(response_wire),
                                                                                  typed_response, response_bytes);
    }
    const auto slave_scope = NA_SCOPE_TERMINAL_SLAVE;
    if (!encoded ||
        !validate_response_resource(client_endpoint, slave_scope, naos::system::TerminalSlave::protocol_uuid,
                                    NA_BINDING_CLIENT_END) ||
        !validate_response_resource(public_job_control, NA_SCOPE_TERMINAL_JOB_CONTROL,
                                    naos::system::TerminalJobControl::protocol_uuid, NA_BINDING_KERNEL_VIEW))
    {
        (void)naos_handle_close(client_endpoint);
        if (public_job_control != NA_HANDLE_INVALID)
            (void)naos_handle_close(public_job_control);
        rollback_server_endpoint(state, server_endpoint);
        discard(NA_OUTCOME_REASON_PROTOCOL_VIOLATION, true);
        return;
    }
    na_resource_disposition_t response_resources[2]{};
    response_resources[0] = {client_endpoint, NA_RESOURCE_MOVE, 0, NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT,
                             NA_SCOPE_TERMINAL_SLAVE};
    response_resources[1] = {public_job_control, NA_RESOURCE_MOVE, 0,
                             NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT, NA_SCOPE_TERMINAL_JOB_CONTROL};
    na_reply_frame_t reply{};
    reply.struct_size = sizeof(reply);
    reply.bytes = reinterpret_cast<std::uint64_t>(response_wire);
    reply.byte_count = response_bytes;
    reply.resources = reinterpret_cast<std::uint64_t>(response_resources);
    reply.resource_count = 2;
    if (_na_responder_reply(responder, &reply) != NA_STATUS_OK)
    {
        (void)naos_handle_close(client_endpoint);
        (void)naos_handle_close(public_job_control);
        rollback_server_endpoint(state, server_endpoint);
        discard(NA_OUTCOME_REASON_PEER_CLOSED, false);
        return;
    }
    (void)naos_handle_close(pending.invocation);
    free(pending.wire);
    pending = {};
    if (state.pending_locator_open_count != 0)
        state.pending_locator_open_count--;
}

void flush_pending_locator_opens(service_state &state)
{
    for (uint64_t i = 0; i < max_pairs; i++)
        if (state.pending_locator_opens[i].active)
            finish_pending_locator_open(state, state.pending_locator_opens[i]);
}

bool queue_driver_action(pty_pair &pair, driver_action_kind kind, std::uint64_t value, na_handle_t responder)
{
    if (pair.driver_control == NA_HANDLE_INVALID || pair.pending_driver_action_count >= 16)
        return false;
    const auto sequence = pair.next_driver_sequence++;
    if (pair.next_driver_sequence == 0)
        pair.next_driver_sequence = 1;
    pair.pending_driver_actions[pair.pending_driver_action_count++] = {kind, value, responder, sequence};
    return true;
}

void finish_driver_action(pty_pair &pair)
{
    if (pair.active_driver_invocation == NA_HANDLE_INVALID)
        return;

    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    if (_na_handle_get_info(pair.active_driver_invocation, &info) != NA_STATUS_OK)
    {
        (void)naos_handle_close(pair.active_driver_invocation);
        pair.active_driver_invocation = NA_HANDLE_INVALID;
        if (pair.active_driver_responder != NA_HANDLE_INVALID)
            reject_responder(pair.active_driver_responder, NA_OUTCOME_REASON_PEER_CLOSED);
        pair.active_driver_responder = NA_HANDLE_INVALID;
        return;
    }
    if ((info.signals & (NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED)) == 0)
        return;

    int error = -EIO;
    if ((info.signals & NA_SIGNAL_COMPLETED) != 0)
    {
        auto transport = make_transport();
        auto client =
            naos::system::TerminalDriverControl::TerminalDriverControlClient(transport.async(), pair.driver_control);
        na_result_frame_t result{};
        na_status_t status = NA_STATUS_INVALID_MESSAGE;
        if (pair.active_driver_action == driver_action_kind::raise_foreground)
        {
            naos::system::TerminalDriverControl::raise_foreground_response response{};
            status = client.take_raise_foreground(pair.active_driver_invocation, response, pair.active_driver_wire,
                                                  sizeof(pair.active_driver_wire), nullptr, 0, result);
        }
        else if (pair.active_driver_action == driver_action_kind::notify_winsize)
        {
            naos::system::TerminalDriverControl::notify_winsize_changed_response response{};
            status =
                client.take_notify_winsize_changed(pair.active_driver_invocation, response, pair.active_driver_wire,
                                                   sizeof(pair.active_driver_wire), nullptr, 0, result);
        }
        else
        {
            naos::system::TerminalDriverControl::hangup_response response{};
            status = client.take_hangup(pair.active_driver_invocation, response, pair.active_driver_wire,
                                        sizeof(pair.active_driver_wire), nullptr, 0, result);
        }
        if (status == NA_STATUS_OK)
            error = driver_result_error(result);
    }
    (void)naos_handle_close(pair.active_driver_invocation);
    pair.active_driver_invocation = NA_HANDLE_INVALID;
    const auto responder = pair.active_driver_responder;
    pair.active_driver_responder = NA_HANDLE_INVALID;
    if (responder == NA_HANDLE_INVALID)
    {
        if (error < 0)
            _s_log("ttyd: queued driver action failed\n");
        return;
    }
    if (error < 0)
    {
        _s_log("ttyd: driver action rejecting responder\n");
        reject_responder(responder, terminal_error_reason(error));
        return;
    }
    na_reply_frame_t reply{};
    reply.struct_size = sizeof(reply);
    if (_na_responder_reply(responder, &reply) != NA_STATUS_OK)
        (void)naos_handle_close(responder);
}

bool submit_driver_action(pty_pair &pair, const pending_driver_action &action)
{
    auto transport = make_transport();
    auto client =
        naos::system::TerminalDriverControl::TerminalDriverControlClient(transport.async(), pair.driver_control);
    na_handle_t invocation = NA_HANDLE_INVALID;
    na_status_t status = NA_STATUS_INVALID_MESSAGE;
    if (action.kind == driver_action_kind::raise_foreground)
    {
        naos::system::TerminalDriverControl::raise_foreground_request request{};
        request.action = static_cast<naos::system::TerminalDriverControl::DriverAction>(action.value);
        status = client.submit_raise_foreground(request, nullptr, 0, &invocation, pair.active_driver_wire,
                                                sizeof(pair.active_driver_wire));
    }
    else if (action.kind == driver_action_kind::notify_winsize)
    {
        naos::system::TerminalDriverControl::notify_winsize_changed_request request{};
        status = client.submit_notify_winsize_changed(request, nullptr, 0, &invocation, pair.active_driver_wire,
                                                      sizeof(pair.active_driver_wire));
    }
    else
    {
        naos::system::TerminalDriverControl::hangup_request request{};
        status = client.submit_hangup(request, nullptr, 0, &invocation, pair.active_driver_wire,
                                      sizeof(pair.active_driver_wire));
    }
    if (status != NA_STATUS_OK)
    {
        return false;
    }
    pair.active_driver_invocation = invocation;
    pair.active_driver_action = action.kind;
    pair.active_driver_sequence = action.sequence;
    pair.active_driver_responder = action.responder;
    return true;
}

void flush_driver_actions(service_state &state)
{
    for (uint64_t i = 0; i < state.pair_count; i++)
    {
        auto &pair = state.pairs[i];
        if (!pair.allocated)
            continue;
        for (uint64_t action_index = 0; action_index < pair.pending_driver_action_count;)
        {
            auto &action = pair.pending_driver_actions[action_index];
            if (action.responder == NA_HANDLE_INVALID || !responder_cancelled_or_closed(action.responder))
            {
                action_index++;
                continue;
            }
            (void)naos_handle_close(action.responder);
            for (uint64_t next = action_index + 1; next < pair.pending_driver_action_count; next++)
                pair.pending_driver_actions[next - 1] = pair.pending_driver_actions[next];
            pair.pending_driver_action_count--;
        }
        finish_driver_action(pair);
        if (pair.active_driver_invocation != NA_HANDLE_INVALID && pair.active_driver_responder != NA_HANDLE_INVALID &&
            responder_cancelled_or_closed(pair.active_driver_responder))
        {
            (void)_na_invocation_cancel(pair.active_driver_invocation);
            (void)naos_handle_close(pair.active_driver_responder);
            pair.active_driver_responder = NA_HANDLE_INVALID;
        }
        if (pair.retiring && pair.active_driver_invocation == NA_HANDLE_INVALID &&
            pair.pending_driver_action_count == 0)
        {
            finalize_pair(state, pair);
            continue;
        }
        if (pair.active_driver_invocation != NA_HANDLE_INVALID || pair.pending_driver_action_count == 0)
            continue;
        const auto action = pair.pending_driver_actions[0];
        for (uint64_t j = 1; j < pair.pending_driver_action_count; j++)
            pair.pending_driver_actions[j - 1] = pair.pending_driver_actions[j];
        pair.pending_driver_action_count--;
        if (action.responder != NA_HANDLE_INVALID && responder_cancelled_or_closed(action.responder))
        {
            (void)naos_handle_close(action.responder);
            continue;
        }
        if (!submit_driver_action(pair, action) && action.responder != NA_HANDLE_INVALID)
            reject_responder(action.responder, NA_OUTCOME_REASON_BROKER_FAILURE);
    }
}

na_outcome_reason_t terminal_error_reason(int error)
{
    if (error == -EAGAIN)
        return NA_OUTCOME_REASON_REQUEST_DISCARDED;
    if (error == -EPIPE)
        return NA_OUTCOME_REASON_PEER_CLOSED;
    if (error == -EIO)
        return NA_OUTCOME_REASON_OBJECT_REVOKED;
    if (error == -ENOTSUP)
        return NA_OUTCOME_REASON_UNSUPPORTED;
    return NA_OUTCOME_REASON_BROKER_FAILURE;
}

int driver_result_error(const na_result_frame_t &result)
{
    if (result.protocol_error < 0)
        return static_cast<int>(result.protocol_error);
    return result.execution_outcome == NA_EXECUTION_NONE ? 0 : -EIO;
}

void reject_responder(na_handle_t responder, na_outcome_reason_t reason = NA_OUTCOME_REASON_REQUEST_DISCARDED)
{
    na_fail_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.execution_outcome = NA_EXECUTION_NOT_DELIVERED;
    frame.outcome_reason = reason;
    (void)_na_responder_fail(responder, &frame);
}

void reject_responder(request_context &context, na_outcome_reason_t reason = NA_OUTCOME_REASON_REQUEST_DISCARDED,
                      std::int64_t protocol_error = 0)
{
    context.outcome = naoidl::dispatch_outcome::failed;
    context.failure_reason = reason;
    if (protocol_error != 0)
        context.failure_error = protocol_error;
    else if (reason == NA_OUTCOME_REASON_UNSUPPORTED)
        context.failure_error = -static_cast<std::int64_t>(ENOTSUP);
    else
        context.failure_error = 0;
}

bool responder_cancelled_or_closed(na_handle_t responder)
{
    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    if (_na_handle_get_info(responder, &info) != NA_STATUS_OK)
        return true;
    return (info.signals & (NA_SIGNAL_CANCEL_REQUESTED | NA_SIGNAL_PEER_CLOSED)) != 0;
}

void mark_pending(request_context &context) { context.outcome = naoidl::dispatch_outcome::pending; }

bool add_pending_read(service_state &state, na_handle_t endpoint, na_handle_t responder, std::size_t size, bool master,
                      bool nonblock = false)
{
    if (state.pending_read_count >= max_endpoints)
    {
        state.quota_rejections++;
        return false;
    }
    uint64_t per_binding = 0;
    for (uint64_t i = 0; i < state.pending_read_count; i++)
        per_binding += state.pending_reads[i].endpoint == endpoint;
    if (per_binding >= max_pending_per_binding)
    {
        state.quota_rejections++;
        return false;
    }
    pending_read pending{endpoint, responder, size, master, 0, false, nonblock};
    auto *binding = find_binding(state, endpoint);
    auto *pair = binding == nullptr ? nullptr : find_pair(state, binding->pair_id);
    if (pair != nullptr && !master)
    {
        const auto attributes = pair->core.get_termios();
        if ((attributes.local_flags & ttyd::termios_lflag::icanon) == 0 &&
            attributes.control_chars[ttyd::termios_cc::vtime] != 0 &&
            (attributes.control_chars[ttyd::termios_cc::vmin] == 0 || pair->core.input_available() != 0))
        {
            pending.timer_started = attributes.control_chars[ttyd::termios_cc::vmin] == 0;
            pending.deadline_ms =
                monotonic_millis() + static_cast<uint64_t>(attributes.control_chars[ttyd::termios_cc::vtime]) * 100;
        }
    }
    state.pending_reads[state.pending_read_count++] = pending;
    return true;
}

void flush_pending_reads(service_state &state)
{
    if (state.pending_read_count == 0)
        return;
    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    std::uint8_t data[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    const auto now = monotonic_millis();
    uint64_t i = 0;
    while (i < state.pending_read_count)
    {
        auto &pending = state.pending_reads[i];
        if (responder_cancelled_or_closed(pending.responder))
        {
            (void)naos_handle_close(pending.responder);
            pending = state.pending_reads[--state.pending_read_count];
            continue;
        }
        auto *binding = find_binding(state, pending.endpoint);
        auto *pair = binding == nullptr ? nullptr : find_pair(state, binding->pair_id);
        if (pair == nullptr)
        {
            (void)naos_handle_close(pending.responder);
            pending = state.pending_reads[--state.pending_read_count];
            continue;
        }

        std::size_t read = 0;
        const std::size_t want =
            pending.size > NA_CHANNEL_MAX_MESSAGE_BYTES ? NA_CHANNEL_MAX_MESSAGE_BYTES : pending.size;
        const bool timeout_expired = pending.deadline_ms != 0 && now >= pending.deadline_ms;
        const int result = pending.master ? pair->core.read_output(data, want, true, &read)
                                          : pair->core.read_input(data, want, true, &read, timeout_expired);
        if (result == -EAGAIN)
        {
            if (pending.nonblock)
            {
                reject_responder(pending.responder, terminal_error_reason(result));
                pending = state.pending_reads[--state.pending_read_count];
                continue;
            }
            if (!pending.master)
            {
                const auto attributes = pair->core.get_termios();
                if ((attributes.local_flags & ttyd::termios_lflag::icanon) == 0 &&
                    attributes.control_chars[ttyd::termios_cc::vmin] != 0 &&
                    attributes.control_chars[ttyd::termios_cc::vtime] != 0 && !pending.timer_started &&
                    pair->core.input_available() != 0)
                {
                    pending.timer_started = true;
                    pending.deadline_ms =
                        now + static_cast<uint64_t>(attributes.control_chars[ttyd::termios_cc::vtime]) * 100;
                }
            }
            i++;
            continue;
        }
        if (result < 0)
        {
            reject_responder(pending.responder, terminal_error_reason(result));
            pending = state.pending_reads[--state.pending_read_count];
            continue;
        }

        std::uint64_t written = 0;
        bool encoded = false;
        if (pending.master)
        {
            naos::system::TerminalMaster::read_response response{};
            response.data = {data, static_cast<std::uint32_t>(read)};
            encoded = naos::system::TerminalMaster::encode_read_response(wire, sizeof(wire), response, written);
        }
        else
        {
            naos::system::TerminalSlave::read_response response{};
            response.data = {data, static_cast<std::uint32_t>(read)};
            encoded = naos::system::TerminalSlave::encode_read_response(wire, sizeof(wire), response, written);
        }
        na_reply_frame_t reply_frame{};
        reply_frame.struct_size = sizeof(reply_frame);
        reply_frame.bytes = written == 0 ? 0 : reinterpret_cast<std::uint64_t>(wire);
        reply_frame.byte_count = written;
        const auto status = encoded ? _na_responder_reply(pending.responder, &reply_frame) : NA_STATUS_INVALID_MESSAGE;
        if (status != NA_STATUS_OK)
            (void)naos_handle_close(pending.responder);
        pending = state.pending_reads[--state.pending_read_count];
    }
}

naoidl::native_transport make_transport()
{
    naoidl::native_transport_api api{};
    api.handle_close = [](void *, na_handle_t handle) { return static_cast<na_status_t>(_na_handle_close(handle)); };
    api.handle_get_info = [](void *, na_handle_t handle, na_handle_info_t *info) {
        return static_cast<na_status_t>(_na_handle_get_info(handle, info));
    };
    api.channel_receive = [](void *, na_handle_t endpoint, na_channel_receive_frame_t *frame) {
        return static_cast<na_status_t>(_na_channel_receive(endpoint, frame));
    };
    api.invoke_submit = [](void *, na_handle_t target, const na_submit_frame_t *frame, na_handle_t *invocation) {
        return static_cast<na_status_t>(_na_invoke_submit(target, frame, invocation));
    };
    api.invocation_take_result = [](void *, na_handle_t invocation, na_result_frame_t *frame) {
        return static_cast<na_status_t>(_na_invocation_take_result(invocation, frame));
    };
    api.responder_reply = [](void *, na_handle_t responder, const na_reply_frame_t *frame) {
        return static_cast<na_status_t>(_na_responder_reply(responder, frame));
    };
    api.responder_fail = [](void *, na_handle_t responder, const na_fail_frame_t *frame) {
        return static_cast<na_status_t>(_na_responder_fail(responder, frame));
    };
    return naoidl::native_transport(api);
}

uint64_t response_resource_count_for(uint64_t scope, uint64_t method_id)
{
    if (scope == NA_SCOPE_TERMINAL_MANAGER)
        return (method_id >= 1 && method_id <= 6) ? 2 : 0;
    if (scope == NA_SCOPE_TERMINAL_MASTER)
        return method_id == 12 || method_id == 19 ? 1 : 0;
    if (scope == NA_SCOPE_TERMINAL_SLAVE)
        return method_id == 11 || method_id == 15 ? 1 : 0;
    return 0;
}

bool valid_terminal_server_endpoint(na_handle_t endpoint, uint64_t scope, const na_uuid_t &uuid, uint64_t revision,
                                    uint64_t features)
{
    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    if (_na_handle_get_info(endpoint, &info) != NA_STATUS_OK)
        return false;
    return info.binding == NA_BINDING_SERVER_END && info.scope == scope && info.revision == revision &&
           info.features == features &&
           std::memcmp(info.protocol_uuid.bytes, uuid.bytes, sizeof(info.protocol_uuid.bytes)) == 0 &&
           (info.meta_rights & (NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT)) ==
               (NA_RIGHT_TRANSFER | NA_RIGHT_WAIT | NA_RIGHT_INSPECT) &&
           (info.protocol_rights & NA_PROTOCOL_RIGHT_INVOKE) != 0;
}

bool valid_terminal_manager_server(na_handle_t endpoint)
{
    return valid_terminal_server_endpoint(
        endpoint, NA_SCOPE_TERMINAL_MANAGER, naos::system::TerminalManager::protocol_uuid,
        naos::system::TerminalManager::revision, naos::system::TerminalManager::features);
}

void close_received_resources(const na_channel_receive_frame_t &frame, const na_handle_t *resources)
{
    if (resources == nullptr)
        return;
    const auto count =
        frame.actual_resources > NA_CHANNEL_MAX_RESOURCES ? NA_CHANNEL_MAX_RESOURCES : frame.actual_resources;
    for (uint64_t i = 0; i < count; i++)
        (void)naos_handle_close(resources[i]);
}

void ttyd_control_event(ttyd::control_event event, void *user_data)
{
    auto *pair = static_cast<pty_pair *>(user_data);
    if (pair == nullptr)
        return;
    const uint64_t action = event == ttyd::control_event::interrupt ? 1 : (event == ttyd::control_event::quit ? 2 : 3);
    (void)queue_driver_action(*pair, driver_action_kind::raise_foreground, action);
}

pty_pair *ensure_console_pair(service_state &state)
{
    auto *pair = find_pair(state, 0);
    if (pair == nullptr || pair->revoked || pair->retiring)
        return nullptr;
    return pair;
}

class terminal_manager_handler
{
  public:
    explicit terminal_manager_handler(service_state &state)
        : state_(state)
    {
    }

    void set_dispatch_context(na_handle_t responder, na_resource_disposition_t *resources, uint64_t resource_count,
                              const na_handle_t *request_resources, uint64_t request_resource_count,
                              uint64_t caller_pid)
    {
        context_ = {responder, resources, resource_count, request_resources, request_resource_count, caller_pid};
    }

    naoidl::dispatch_outcome dispatch_outcome() const { return context_.outcome; }
    na_outcome_reason_t dispatch_failure_reason() const { return context_.failure_reason; }
    std::int64_t dispatch_failure_error() const { return context_.failure_error; }
    void rollback_dispatch()
    {
        if (transaction_endpoint_ != NA_HANDLE_INVALID)
            rollback_server_endpoint(state_, transaction_endpoint_);
        transaction_endpoint_ = NA_HANDLE_INVALID;
    }
    void commit_dispatch() { transaction_endpoint_ = NA_HANDLE_INVALID; }

    bool create_pty(const naos::system::TerminalManager::create_pty_request &request,
                    naos::system::TerminalManager::create_pty_response &response)
    {
        (void)response;
        if (ttyd::terminal_core::validate_termios(to_ttyd_termios(request.attributes)) != 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE, -EINVAL);
            return false;
        }
        uint64_t allocated_pairs = 0;
        for (uint64_t i = 0; i < state_.pair_count; i++)
            allocated_pairs += state_.pairs[i].allocated;
        if (!valid_terminal_mode(request.mode) || state_.endpoint_count >= max_endpoints ||
            state_.binding_count >= max_endpoints || allocated_pairs + state_.pending_create_count >= max_pairs)
            return false;
        if (!queue_pending_create(state_, request, context_.responder))
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        mark_pending(context_);
        return false;
    }

    bool open_pty_slave(const naos::system::TerminalManager::open_pty_slave_request &request,
                        naos::system::TerminalManager::open_pty_slave_response &response)
    {
        if (state_.endpoint_count >= max_endpoints || state_.binding_count >= max_endpoints)
            return false;
        auto *pair = find_pair(state_, request.locator.pair_id);
        if (!valid_terminal_mode(request.mode) || pair == nullptr || pair->generation != request.locator.generation ||
            pair->slave_locked || !pair->slave_granted || pair->revoked || pair->retiring ||
            pair->locator_token != request.locator.token)
            return false;
        if (!queue_locator_validation(state_, pending_locator_kind::slave_locator, pair->id, pair->generation,
                                      pair->locator_token, request.mode, context_.caller_pid, context_.responder))
            return false;
        mark_pending(context_);
        return false;
    }

    bool open_pty_slave_by_number(const naos::system::TerminalManager::open_pty_slave_by_number_request &request,
                                  naos::system::TerminalManager::open_pty_slave_by_number_response &response)
    {
        if (state_.endpoint_count >= max_endpoints || state_.binding_count >= max_endpoints ||
            !valid_terminal_mode(request.mode))
            return false;
        auto *pair = find_pair(state_, request.number);
        if (pair == nullptr || pair->slave_locked || !pair->slave_granted || pair->revoked || pair->retiring)
            return false;
        if (!queue_locator_validation(state_, pending_locator_kind::slave_by_number, pair->id, pair->generation,
                                      pair->locator_token, request.mode, context_.caller_pid, context_.responder))
            return false;
        mark_pending(context_);
        return false;
    }

    bool open_console(const naos::system::TerminalManager::open_console_request &request,
                      naos::system::TerminalManager::open_console_response &response)
    {
        if (!valid_terminal_mode(request.mode) || state_.endpoint_count >= max_endpoints ||
            state_.binding_count >= max_endpoints)
            return false;
        auto *pair = ensure_console_pair(state_);
        if (pair == nullptr)
        {
            naos::system::TerminalManager::create_pty_request create_request{};
            create_request.mode = request.mode;
            if (!queue_pending_create(state_, create_request, context_.responder, pending_create_kind::console, false))
                reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
            else
                mark_pending(context_);
            return false;
        }
        na_handle_t client = NA_HANDLE_INVALID;
        na_handle_t server = NA_HANDLE_INVALID;
        if (!create_endpoint_pair(state_.slave_descriptor, client, server))
            return false;
        if (!add_server_endpoint(state_, server, pair->id, false, request.mode))
        {
            (void)naos_handle_close(client);
            reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
            return false;
        }
        transaction_endpoint_ = server;
        response.slave.value = 0;
        response.job_control.value = 1;
        if (!set_two_response_resources(context_, client, public_job_control_handle(pair->job_control),
                                        NA_SCOPE_TERMINAL_SLAVE, request.mode))
        {
            rollback_server_endpoint(state_, server);
            return false;
        }
        return true;
    }

    bool open_console_master(const naos::system::TerminalManager::open_console_master_request &request,
                             naos::system::TerminalManager::open_console_master_response &response)
    {
        if (context_.request_resource_count != 1 || request.frontend.value != 0 ||
            context_.request_resources == nullptr)
            return false;
        na_handle_info_t frontend_info{};
        frontend_info.struct_size = sizeof(frontend_info);
        if (_na_handle_get_info(context_.request_resources[0], &frontend_info) != NA_STATUS_OK ||
            frontend_info.binding != NA_BINDING_KERNEL_VIEW ||
            (frontend_info.meta_rights & (NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT)) !=
                (NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT) ||
            (frontend_info.scope != NA_SCOPE_NONE) ||
            (frontend_info.protocol_rights & NA_DISPLAY_RIGHT_WRITER) == 0)
            return false;
        if (!valid_terminal_mode(request.mode) || state_.endpoint_count >= max_endpoints ||
            state_.binding_count >= max_endpoints)
            return false;
        auto *pair = ensure_console_pair(state_);
        if (pair == nullptr)
        {
            naos::system::TerminalManager::create_pty_request create_request{};
            create_request.mode = request.mode;
            if (!queue_pending_create(state_, create_request, context_.responder, pending_create_kind::console, true))
                reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
            else
                mark_pending(context_);
            return false;
        }
        na_handle_t client = NA_HANDLE_INVALID;
        na_handle_t server = NA_HANDLE_INVALID;
        if (!create_endpoint_pair(state_.master_descriptor, client, server))
            return false;
        if (!add_server_endpoint(state_, server, pair->id, true, request.mode))
        {
            (void)naos_handle_close(client);
            reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
            return false;
        }
        transaction_endpoint_ = server;
        response.master.value = 0;
        response.job_control.value = 1;
        if (!set_two_response_resources(context_, client, public_job_control_handle(pair->job_control),
                                        NA_SCOPE_TERMINAL_MASTER, request.mode))
        {
            rollback_server_endpoint(state_, server);
            return false;
        }
        return true;
    }

    bool open_controlling(const naos::system::TerminalManager::open_controlling_request &request,
                          naos::system::TerminalManager::open_controlling_response &response)
    {
        if (state_.endpoint_count >= max_endpoints || state_.binding_count >= max_endpoints)
            return false;
        auto *pair = find_pair(state_, request.locator.pair_id);
        if (!valid_terminal_mode(request.mode) || pair == nullptr || pair->generation != request.locator.generation ||
            pair->slave_locked || pair->revoked || pair->retiring || pair->locator_token != request.locator.token ||
            !pair->slave_granted)
            return false;
        if (!queue_locator_validation(state_, pending_locator_kind::controlling, pair->id, pair->generation,
                                      pair->locator_token, request.mode, context_.caller_pid, context_.responder))
            return false;
        mark_pending(context_);
        return false;
    }

  private:
    service_state &state_;
    request_context context_{};
    na_handle_t transaction_endpoint_ = NA_HANDLE_INVALID;
};

naos::system::TerminalMaster::Readiness master_readiness(service_state &state, na_handle_t endpoint)
{
    auto *pair = find_pair(state, find_binding(state, endpoint)->pair_id);
    naos::system::TerminalMaster::Readiness result{};
    result.ready_mask = pair->core.master_poll_events();
    result.hangup_mask = pair->core.slave_hung_up() ? ttyd::tty_poll::hangup : 0;
    result.generation = pair->core.generation();
    return result;
}

naos::system::TerminalSlave::Readiness slave_readiness(service_state &state, na_handle_t endpoint)
{
    auto *pair = find_pair(state, find_binding(state, endpoint)->pair_id);
    naos::system::TerminalSlave::Readiness result{};
    result.ready_mask = pair->core.slave_poll_events();
    result.hangup_mask = pair->core.master_hung_up() ? ttyd::tty_poll::hangup : 0;
    result.generation = pair->core.generation();
    return result;
}

bool add_pending_watch(service_state &state, na_handle_t endpoint, na_handle_t responder, uint64_t mask,
                       uint64_t observed_generation, bool master)
{
    if (state.pending_watch_count >= max_endpoints)
    {
        state.quota_rejections++;
        return false;
    }
    uint64_t per_binding = 0;
    for (uint64_t i = 0; i < state.pending_watch_count; i++)
        per_binding += state.pending_watches[i].endpoint == endpoint;
    if (per_binding >= max_pending_per_binding)
    {
        state.quota_rejections++;
        return false;
    }
    state.pending_watches[state.pending_watch_count++] = {endpoint, responder, mask, observed_generation, master};
    return true;
}

void flush_pending_watches(service_state &state)
{
    if (state.pending_watch_count == 0)
        return;
    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    uint64_t i = 0;
    while (i < state.pending_watch_count)
    {
        auto &pending = state.pending_watches[i];
        if (responder_cancelled_or_closed(pending.responder))
        {
            (void)naos_handle_close(pending.responder);
            pending = state.pending_watches[--state.pending_watch_count];
            continue;
        }
        auto *binding = find_binding(state, pending.endpoint);
        auto *pair = binding == nullptr ? nullptr : find_pair(state, binding->pair_id);
        if (pair == nullptr)
        {
            (void)naos_handle_close(pending.responder);
            pending = state.pending_watches[--state.pending_watch_count];
            continue;
        }

        bool satisfied = false;
        std::uint64_t written = 0;
        bool encoded = false;
        if (pending.master)
        {
            const auto readiness = master_readiness(state, pending.endpoint);
            satisfied = readiness.generation != pending.observed_generation ||
                        (readiness.ready_mask & pending.mask) != 0 || readiness.hangup_mask != 0;
            if (satisfied)
            {
                naos::system::TerminalMaster::watch_response response{};
                response.readiness = readiness;
                encoded = naos::system::TerminalMaster::encode_watch_response(wire, sizeof(wire), response, written);
            }
        }
        else
        {
            const auto readiness = slave_readiness(state, pending.endpoint);
            satisfied = readiness.generation != pending.observed_generation ||
                        (readiness.ready_mask & pending.mask) != 0 || readiness.hangup_mask != 0;
            if (satisfied)
            {
                naos::system::TerminalSlave::watch_response response{};
                response.readiness = readiness;
                encoded = naos::system::TerminalSlave::encode_watch_response(wire, sizeof(wire), response, written);
            }
        }
        if (!satisfied)
        {
            i++;
            continue;
        }

        na_reply_frame_t reply_frame{};
        reply_frame.struct_size = sizeof(reply_frame);
        reply_frame.bytes = written == 0 ? 0 : reinterpret_cast<std::uint64_t>(wire);
        reply_frame.byte_count = written;
        const auto status = encoded ? _na_responder_reply(pending.responder, &reply_frame) : NA_STATUS_INVALID_MESSAGE;
        if (status != NA_STATUS_OK)
            (void)naos_handle_close(pending.responder);
        pending = state.pending_watches[--state.pending_watch_count];
    }
}

class terminal_master_handler
{
  public:
    explicit terminal_master_handler(service_state &state, na_handle_t endpoint)
        : state_(state)
        , endpoint_(endpoint)
    {
    }

    void set_dispatch_context(na_handle_t responder, na_resource_disposition_t *resources, uint64_t resource_count,
                              const na_handle_t *request_resources, uint64_t request_resource_count,
                              uint64_t caller_pid)
    {
        context_ = {responder, resources, resource_count, request_resources, request_resource_count, caller_pid};
    }

    naoidl::dispatch_outcome dispatch_outcome() const { return context_.outcome; }
    na_outcome_reason_t dispatch_failure_reason() const { return context_.failure_reason; }
    std::int64_t dispatch_failure_error() const { return context_.failure_error; }
    void rollback_dispatch()
    {
        if (transaction_endpoint_ != NA_HANDLE_INVALID)
            rollback_server_endpoint(state_, transaction_endpoint_);
        transaction_endpoint_ = NA_HANDLE_INVALID;
    }
    void commit_dispatch() { transaction_endpoint_ = NA_HANDLE_INVALID; }

    bool read(const naos::system::TerminalMaster::read_request &request,
              naos::system::TerminalMaster::read_response &response)
    {
        auto *binding = find_binding(state_, endpoint_);
        if (binding == nullptr || (binding->mode & 1) == 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        auto *pair = find_pair(state_, binding->pair_id);
        const std::size_t want =
            request.size > NA_CHANNEL_MAX_MESSAGE_BYTES ? NA_CHANNEL_MAX_MESSAGE_BYTES : request.size;
        std::size_t read = 0;
        const auto *description = find_open_description(state_, binding->open_description);
        const bool nonblock = description != nullptr && (description->status_flags & terminal_status_nonblock) != 0;
        const auto result = pair->core.read_output(response_buffer_, want, nonblock, &read);
        if (result == -EAGAIN)
        {
            if (nonblock)
            {
                reject_responder(context_, terminal_error_reason(result));
                return false;
            }
            if (!add_pending_read(state_, endpoint_, context_.responder, want, true))
                reject_responder(context_);
            else
                mark_pending(context_);
            return false;
        }
        if (result < 0)
        {
            reject_responder(context_, terminal_error_reason(result));
            return false;
        }
        // The generated dispatcher encodes the response after this handler
        // returns. Keep the bytes in handler-owned storage until that encode
        // completes; a function-local buffer would already be dead here.
        response.data = {response_buffer_, static_cast<std::uint32_t>(read)};
        flush_pending_writes(state_);
        flush_pending_attributes(state_);
        return true;
    }

    bool write(const naos::system::TerminalMaster::write_request &request,
               naos::system::TerminalMaster::write_response &response)
    {
        auto *binding = find_binding(state_, endpoint_);
        if (binding == nullptr || (binding->mode & 2) == 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        auto *pair = find_pair(state_, binding->pair_id);
        const auto *description = find_open_description(state_, binding->open_description);
        const bool nonblock = description != nullptr && (description->status_flags & terminal_status_nonblock) != 0;
        if (request.data.size == 0)
        {
            response.count = 0;
            return true;
        }

        std::uint8_t *owned = nullptr;
        if (!nonblock)
        {
            owned = static_cast<std::uint8_t *>(malloc(request.data.size));
            if (owned == nullptr)
            {
                reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
                return false;
            }
            std::memcpy(owned, request.data.data, request.data.size);
        }
        std::size_t written = 0;
        const auto result =
            pair->core.receive_input(nonblock ? request.data.data : owned, request.data.size, true, &written);
        if (result < 0 && (result != -EAGAIN || written == 0) && nonblock)
        {
            reject_responder(context_, terminal_error_reason(result));
            return false;
        }
        if (!nonblock && written < request.data.size)
        {
            if (result < 0 && result != -EAGAIN)
            {
                free(owned);
                if (written != 0)
                {
                    response.count = written;
                    flush_pending_reads(state_);
                    return true;
                }
                reject_responder(context_, terminal_error_reason(result));
                return false;
            }
            if (!add_pending_write(state_, endpoint_, context_.responder, owned, request.data.size, written, true))
            {
                // The prefix was already committed to the PTY.  Returning a
                // short count is the only lossless result when the bounded
                // pending queue cannot accept the remainder.
                if (written != 0)
                {
                    response.count = written;
                    flush_pending_reads(state_);
                    return true;
                }
                reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
                return false;
            }
            mark_pending(context_);
            flush_pending_reads(state_);
            return false;
        }
        free(owned);
        response.count = written;
        flush_pending_reads(state_);
        flush_pending_writes(state_);
        return true;
    }

    bool get_attributes(const naos::system::TerminalMaster::get_attributes_request &,
                        naos::system::TerminalMaster::get_attributes_response &response)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        response.attributes = to_idl_termios<naos::system::TerminalMaster::Termios>(pair->core.get_termios());
        return true;
    }

    bool set_attributes(const naos::system::TerminalMaster::set_attributes_request &request,
                        naos::system::TerminalMaster::set_attributes_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        if (request.action > 2)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        const auto attributes = to_ttyd_termios(request.attributes);
        if (ttyd::terminal_core::validate_termios(attributes) != 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE, -EINVAL);
            return false;
        }
        if (pair->pending_attribute_change.active)
        {
            reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
            return false;
        }
        if ((request.action == 1 || request.action == 2) && pair->core.output_available() != 0)
        {
            pair->pending_attribute_change = {true, endpoint_, context_.responder, attributes, request.action == 2};
            mark_pending(context_);
            return false;
        }
        if (request.action == 2)
            (void)pair->core.flush(ttyd::tty_flush::input);
        (void)pair->core.set_termios(attributes);
        flush_pending_reads(state_);
        flush_pending_attributes(state_);
        flush_pending_watches(state_);
        return true;
    }

    bool get_winsize(const naos::system::TerminalMaster::get_winsize_request &,
                     naos::system::TerminalMaster::get_winsize_response &response)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        response.size = to_idl_winsize<naos::system::TerminalMaster::Winsize>(pair->core.get_winsize());
        return true;
    }

    bool set_winsize(const naos::system::TerminalMaster::set_winsize_request &request,
                     naos::system::TerminalMaster::set_winsize_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        // The size is committed before the downstream notification.  Keep
        // the client RPC completion independent from the notification action;
        // the ordered driver queue still reports notification failures via
        // ttyd supervision/logging and cannot strand the caller if the
        // foreground process disappears while the signal is delivered.
        if (!queue_driver_action(*pair, driver_action_kind::notify_winsize, 0, context_.responder))
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        pair->core.set_winsize(to_ttyd_winsize(request.size));
        mark_pending(context_);
        return false;
    }

    bool flush(const naos::system::TerminalMaster::flush_request &request,
               naos::system::TerminalMaster::flush_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        const int result = pair->core.flush(static_cast<int>(request.queue));
        if (result < 0)
        {
            reject_responder(context_, terminal_error_reason(result));
            return false;
        }
        flush_pending_reads(state_);
        return true;
    }

    bool get_input_count(const naos::system::TerminalMaster::get_input_count_request &,
                         naos::system::TerminalMaster::get_input_count_response &response)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        response.count = pair->core.output_available();
        return true;
    }

    bool send_break(const naos::system::TerminalMaster::send_break_request &request,
                    naos::system::TerminalMaster::send_break_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        const int result = pair->core.send_break(static_cast<std::uint32_t>(request.duration_ms));
        if (result < 0)
        {
            reject_responder(context_, terminal_error_reason(result));
            return false;
        }
        flush_pending_watches(state_);
        return true;
    }

    bool set_flow(const naos::system::TerminalMaster::set_flow_request &request,
                  naos::system::TerminalMaster::set_flow_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        const int result = pair->core.set_flow(request.action);
        if (result < 0)
        {
            reject_responder(context_, terminal_error_reason(result));
            return false;
        }
        flush_pending_watches(state_);
        return true;
    }

    bool grant_slave(const naos::system::TerminalMaster::grant_slave_request &,
                     naos::system::TerminalMaster::grant_slave_response &)
    {
        auto *binding = find_binding(state_, endpoint_);
        auto *pair = binding == nullptr || !binding->master ? nullptr : find_pair(state_, binding->pair_id);
        if (pair == nullptr || pair->revoked || pair->retiring)
            return false;
        pair->slave_granted = true;
        pair->slave_mode = 0620;
        pair->core.notify_external_change();
        flush_pending_watches(state_);
        return true;
    }

    bool query(const naos::system::TerminalMaster::query_request &,
               naos::system::TerminalMaster::query_response &response)
    {
        response.readiness = master_readiness(state_, endpoint_);
        return true;
    }

    bool watch(const naos::system::TerminalMaster::watch_request &request,
               naos::system::TerminalMaster::watch_response &response)
    {
        const auto readiness = master_readiness(state_, endpoint_);
        if (readiness.generation != request.observed_generation || (readiness.ready_mask & request.mask) != 0 ||
            readiness.hangup_mask != 0)
        {
            response.readiness = readiness;
            return true;
        }
        if (!add_pending_watch(state_, endpoint_, context_.responder, request.mask, request.observed_generation, true))
            reject_responder(context_);
        else
            mark_pending(context_);
        return false;
    }

    bool clone_binding(const naos::system::TerminalMaster::clone_binding_request &request,
                       naos::system::TerminalMaster::clone_binding_response &response)
    {
        if (state_.endpoint_count >= max_endpoints || state_.binding_count >= max_endpoints)
            return false;
        auto *original = find_binding(state_, endpoint_);
        if (original == nullptr)
            return false;
        const auto requested_rights = request.flags & clone_data_rights;
        auto requested_mode =
            requested_rights == 0 ? original->mode : requested_rights | (original->mode & terminal_status_nonblock);
        if ((request.flags & clone_share_open_description) == 0)
            requested_mode &= ~terminal_status_nonblock;
        if ((request.flags & ~(clone_data_rights | clone_share_open_description | clone_initial_nonblock)) != 0 ||
            requested_mode == 0 || (requested_mode & ~original->mode) != 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        na_handle_t client = NA_HANDLE_INVALID;
        na_handle_t server = NA_HANDLE_INVALID;
        if (!create_endpoint_pair(state_.master_descriptor, client, server))
            return false;
        const auto open_description =
            (request.flags & clone_share_open_description) != 0 ? original->open_description : invalid_open_description;
        const auto initial_mode = (request.flags & clone_initial_nonblock) != 0 ? terminal_status_nonblock : 0;
        if (!add_server_endpoint(state_, server, original->pair_id, true, requested_mode | initial_mode,
                                 open_description))
        {
            (void)naos_handle_close(client);
            reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
            return false;
        }
        transaction_endpoint_ = server;
        response.endpoint.value = 0;
        if (!set_response_resource(context_, client, NA_SCOPE_TERMINAL_MASTER,
                                   naos::system::TerminalMaster::protocol_uuid, NA_BINDING_CLIENT_END, requested_mode))
        {
            rollback_server_endpoint(state_, server);
            return false;
        }
        return true;
    }

    bool get_status_flags(const naos::system::TerminalMaster::get_status_flags_request &,
                          naos::system::TerminalMaster::get_status_flags_response &response)
    {
        const auto *binding = find_binding(state_, endpoint_);
        const auto *description =
            binding == nullptr ? nullptr : find_open_description(state_, binding->open_description);
        if (description == nullptr)
            return false;
        response.flags = description->status_flags;
        return true;
    }

    bool set_status_flags(const naos::system::TerminalMaster::set_status_flags_request &request,
                          naos::system::TerminalMaster::set_status_flags_response &)
    {
        const auto *binding = find_binding(state_, endpoint_);
        auto *description = binding == nullptr ? nullptr : find_open_description(state_, binding->open_description);
        if (description == nullptr)
            return false;
        description->status_flags = request.flags & terminal_status_nonblock;
        auto *pair = find_pair(state_, binding->pair_id);
        if (pair == nullptr)
            return false;
        pair->core.notify_external_change();
        flush_pending_watches(state_);
        return true;
    }

    bool shutdown_input(const naos::system::TerminalMaster::shutdown_input_request &,
                        naos::system::TerminalMaster::shutdown_input_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        pair->core.shutdown_input();
        flush_pending_reads(state_);
        return true;
    }

    bool get_number(const naos::system::TerminalMaster::get_number_request &,
                    naos::system::TerminalMaster::get_number_response &response)
    {
        response.number = find_binding(state_, endpoint_)->pair_id;
        return true;
    }

    bool unlock(const naos::system::TerminalMaster::unlock_request &request,
                naos::system::TerminalMaster::unlock_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        pair->slave_locked = request.locked != 0;
        pair->core.notify_external_change();
        flush_pending_watches(state_);
        return true;
    }

    bool get_slave_locator(const naos::system::TerminalMaster::get_slave_locator_request &,
                           naos::system::TerminalMaster::get_slave_locator_response &response)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        response.pair_id = pair->id;
        response.generation = pair->generation;
        response.token = pair->locator_token;
        return true;
    }

    bool get_job_control(const naos::system::TerminalMaster::get_job_control_request &,
                         naos::system::TerminalMaster::get_job_control_response &response)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        const auto job_control = public_job_control_handle(pair->job_control);
        if (job_control == NA_HANDLE_INVALID)
            return false;
        response.job_control.value = 0;
        return set_response_resource(context_, job_control, NA_SCOPE_TERMINAL_JOB_CONTROL,
                                     naos::system::TerminalJobControl::protocol_uuid, NA_BINDING_KERNEL_VIEW);
    }

  private:
    service_state &state_;
    na_handle_t endpoint_;
    request_context context_{};
    na_handle_t transaction_endpoint_ = NA_HANDLE_INVALID;
    std::uint8_t response_buffer_[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
};

class terminal_slave_handler
{
  public:
    explicit terminal_slave_handler(service_state &state, na_handle_t endpoint)
        : state_(state)
        , endpoint_(endpoint)
    {
    }

    void set_dispatch_context(na_handle_t responder, na_resource_disposition_t *resources, uint64_t resource_count,
                              const na_handle_t *request_resources, uint64_t request_resource_count,
                              uint64_t caller_pid)
    {
        context_ = {responder, resources, resource_count, request_resources, request_resource_count, caller_pid};
    }

    naoidl::dispatch_outcome dispatch_outcome() const { return context_.outcome; }
    na_outcome_reason_t dispatch_failure_reason() const { return context_.failure_reason; }
    std::int64_t dispatch_failure_error() const { return context_.failure_error; }
    void rollback_dispatch()
    {
        if (transaction_endpoint_ != NA_HANDLE_INVALID)
            rollback_server_endpoint(state_, transaction_endpoint_);
        transaction_endpoint_ = NA_HANDLE_INVALID;
    }
    void commit_dispatch() { transaction_endpoint_ = NA_HANDLE_INVALID; }

    bool read(const naos::system::TerminalSlave::read_request &request,
              naos::system::TerminalSlave::read_response &response)
    {
        auto *binding = find_binding(state_, endpoint_);
        if (binding == nullptr || (binding->mode & 1) == 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        const std::size_t want =
            request.size > NA_CHANNEL_MAX_MESSAGE_BYTES ? NA_CHANNEL_MAX_MESSAGE_BYTES : request.size;
        const auto *description = find_open_description(state_, binding->open_description);
        const bool nonblock = description != nullptr && (description->status_flags & terminal_status_nonblock) != 0;
        if (want == 0)
        {
            response.data = {};
            return true;
        }
        if (!add_pending_read(state_, endpoint_, context_.responder, want, false, nonblock))
        {
            reject_responder(context_);
            return false;
        }
        mark_pending(context_);
        flush_pending_reads(state_);
        return false;
    }

    bool write(const naos::system::TerminalSlave::write_request &request,
               naos::system::TerminalSlave::write_response &response)
    {
        auto *binding = find_binding(state_, endpoint_);
        if (binding == nullptr || (binding->mode & 2) == 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        const auto *description = find_open_description(state_, binding->open_description);
        const bool nonblock = description != nullptr && (description->status_flags & terminal_status_nonblock) != 0;
        if (request.data.size == 0)
        {
            response.count = 0;
            return true;
        }
        auto *owned = static_cast<std::uint8_t *>(malloc(request.data.size));
        if (owned == nullptr)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        std::memcpy(owned, request.data.data, request.data.size);
        if (!add_pending_write(state_, endpoint_, context_.responder, owned, request.data.size, 0, false, nonblock))
        {
            reject_responder(context_);
            return false;
        }
        mark_pending(context_);
        flush_pending_writes(state_);
        return false;
    }

    bool get_attributes(const naos::system::TerminalSlave::get_attributes_request &,
                        naos::system::TerminalSlave::get_attributes_response &response)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        response.attributes = to_idl_termios<naos::system::TerminalSlave::Termios>(pair->core.get_termios());
        return true;
    }

    bool set_attributes(const naos::system::TerminalSlave::set_attributes_request &request,
                        naos::system::TerminalSlave::set_attributes_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        if (request.action > 2)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        const auto attributes = to_ttyd_termios(request.attributes);
        if (ttyd::terminal_core::validate_termios(attributes) != 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE, -EINVAL);
            return false;
        }
        if (pair->pending_attribute_change.active)
        {
            reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
            return false;
        }
        if ((request.action == 1 || request.action == 2) && pair->core.output_available() != 0)
        {
            pair->pending_attribute_change = {true, endpoint_, context_.responder, attributes, request.action == 2};
            mark_pending(context_);
            return false;
        }
        if (request.action == 2)
            (void)pair->core.flush(ttyd::tty_flush::input);
        (void)pair->core.set_termios(attributes);
        flush_pending_reads(state_);
        flush_pending_attributes(state_);
        flush_pending_watches(state_);
        return true;
    }

    bool get_winsize(const naos::system::TerminalSlave::get_winsize_request &,
                     naos::system::TerminalSlave::get_winsize_response &response)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        response.size = to_idl_winsize<naos::system::TerminalSlave::Winsize>(pair->core.get_winsize());
        return true;
    }

    bool set_winsize(const naos::system::TerminalSlave::set_winsize_request &request,
                     naos::system::TerminalSlave::set_winsize_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        if (!queue_driver_action(*pair, driver_action_kind::notify_winsize, 0, context_.responder))
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        pair->core.set_winsize(to_ttyd_winsize(request.size));
        mark_pending(context_);
        return false;
    }

    bool flush(const naos::system::TerminalSlave::flush_request &request, naos::system::TerminalSlave::flush_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        const int result = pair->core.flush(static_cast<int>(request.queue));
        if (result < 0)
        {
            reject_responder(context_, terminal_error_reason(result));
            return false;
        }
        flush_pending_reads(state_);
        return true;
    }

    bool get_input_count(const naos::system::TerminalSlave::get_input_count_request &,
                         naos::system::TerminalSlave::get_input_count_response &response)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        response.count = pair->core.input_available();
        return true;
    }

    bool send_break(const naos::system::TerminalSlave::send_break_request &request,
                    naos::system::TerminalSlave::send_break_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        const int result = pair->core.send_break(static_cast<std::uint32_t>(request.duration_ms));
        if (result < 0)
        {
            reject_responder(context_, terminal_error_reason(result));
            return false;
        }
        flush_pending_watches(state_);
        return true;
    }

    bool set_flow(const naos::system::TerminalSlave::set_flow_request &request,
                  naos::system::TerminalSlave::set_flow_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        const int result = pair->core.set_flow(request.action);
        if (result < 0)
        {
            reject_responder(context_, terminal_error_reason(result));
            return false;
        }
        flush_pending_watches(state_);
        return true;
    }

    bool query(const naos::system::TerminalSlave::query_request &,
               naos::system::TerminalSlave::query_response &response)
    {
        response.readiness = slave_readiness(state_, endpoint_);
        return true;
    }

    bool watch(const naos::system::TerminalSlave::watch_request &request,
               naos::system::TerminalSlave::watch_response &response)
    {
        const auto readiness = slave_readiness(state_, endpoint_);
        if (readiness.generation != request.observed_generation || (readiness.ready_mask & request.mask) != 0 ||
            readiness.hangup_mask != 0)
        {
            response.readiness = readiness;
            return true;
        }
        if (!add_pending_watch(state_, endpoint_, context_.responder, request.mask, request.observed_generation, false))
            reject_responder(context_);
        else
            mark_pending(context_);
        return false;
    }

    bool clone_binding(const naos::system::TerminalSlave::clone_binding_request &request,
                       naos::system::TerminalSlave::clone_binding_response &response)
    {
        if (state_.endpoint_count >= max_endpoints || state_.binding_count >= max_endpoints)
            return false;
        auto *original = find_binding(state_, endpoint_);
        if (original == nullptr)
            return false;
        const auto requested_rights = request.flags & clone_data_rights;
        auto requested_mode =
            requested_rights == 0 ? original->mode : requested_rights | (original->mode & terminal_status_nonblock);
        if ((request.flags & clone_share_open_description) == 0)
            requested_mode &= ~terminal_status_nonblock;
        if ((request.flags & ~(clone_data_rights | clone_share_open_description | clone_initial_nonblock)) != 0 ||
            requested_mode == 0 || (requested_mode & ~original->mode) != 0)
        {
            reject_responder(context_, NA_OUTCOME_REASON_BROKER_FAILURE);
            return false;
        }
        na_handle_t client = NA_HANDLE_INVALID;
        na_handle_t server = NA_HANDLE_INVALID;
        if (!create_endpoint_pair(state_.slave_descriptor, client, server))
            return false;
        const auto open_description =
            (request.flags & clone_share_open_description) != 0 ? original->open_description : invalid_open_description;
        const auto initial_mode = (request.flags & clone_initial_nonblock) != 0 ? terminal_status_nonblock : 0;
        if (!add_server_endpoint(state_, server, original->pair_id, false, requested_mode | initial_mode,
                                 open_description))
        {
            (void)naos_handle_close(client);
            reject_responder(context_, NA_OUTCOME_REASON_REQUEST_DISCARDED);
            return false;
        }
        transaction_endpoint_ = server;
        response.endpoint.value = 0;
        if (!set_response_resource(context_, client, NA_SCOPE_TERMINAL_SLAVE,
                                   naos::system::TerminalSlave::protocol_uuid, NA_BINDING_CLIENT_END, requested_mode))
        {
            rollback_server_endpoint(state_, server);
            return false;
        }
        return true;
    }

    bool get_status_flags(const naos::system::TerminalSlave::get_status_flags_request &,
                          naos::system::TerminalSlave::get_status_flags_response &response)
    {
        const auto *binding = find_binding(state_, endpoint_);
        const auto *description =
            binding == nullptr ? nullptr : find_open_description(state_, binding->open_description);
        if (description == nullptr)
            return false;
        response.flags = description->status_flags;
        return true;
    }

    bool set_status_flags(const naos::system::TerminalSlave::set_status_flags_request &request,
                          naos::system::TerminalSlave::set_status_flags_response &)
    {
        const auto *binding = find_binding(state_, endpoint_);
        auto *description = binding == nullptr ? nullptr : find_open_description(state_, binding->open_description);
        if (description == nullptr)
            return false;
        description->status_flags = request.flags & terminal_status_nonblock;
        auto *pair = find_pair(state_, binding->pair_id);
        if (pair == nullptr)
            return false;
        pair->core.notify_external_change();
        flush_pending_watches(state_);
        return true;
    }

    bool shutdown_input(const naos::system::TerminalSlave::shutdown_input_request &,
                        naos::system::TerminalSlave::shutdown_input_response &)
    {
        auto *pair = find_pair(state_, find_binding(state_, endpoint_)->pair_id);
        pair->core.shutdown_input();
        flush_pending_reads(state_);
        return true;
    }

    bool get_job_control(const naos::system::TerminalSlave::get_job_control_request &,
                         naos::system::TerminalSlave::get_job_control_response &response)
    {
        auto *binding = find_binding(state_, endpoint_);
        auto *pair = binding == nullptr ? nullptr : find_pair(state_, binding->pair_id);
        if (pair == nullptr)
            return false;
        const auto job_control = public_job_control_handle(pair->job_control);
        if (job_control == NA_HANDLE_INVALID)
            return false;
        response.job_control.value = 0;
        return set_response_resource(context_, job_control, NA_SCOPE_TERMINAL_JOB_CONTROL,
                                     naos::system::TerminalJobControl::protocol_uuid, NA_BINDING_KERNEL_VIEW);
    }

  private:
    service_state &state_;
    na_handle_t endpoint_;
    request_context context_{};
    na_handle_t transaction_endpoint_ = NA_HANDLE_INVALID;
};

int dispatch_one(service_state &state, na_handle_t endpoint, na_channel_receive_frame_t &frame,
                 std::uint8_t *request_bytes, na_handle_t *request_resources, std::uint8_t *response_bytes,
                 na_resource_disposition_t *response_resources)
{
    auto reject_protocol = [&]() {
        if (frame.responder != NA_HANDLE_INVALID)
        {
            na_fail_frame_t failure{};
            failure.struct_size = sizeof(failure);
            failure.execution_outcome = NA_EXECUTION_NOT_DELIVERED;
            failure.outcome_reason = NA_OUTCOME_REASON_PROTOCOL_VIOLATION;
            (void)_na_responder_fail(frame.responder, &failure);
        }
        (void)naos_handle_close(endpoint);
        return static_cast<int>(NA_STATUS_WRONG_BINDING);
    };

    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    if (_na_handle_get_info(endpoint, &info) != NA_STATUS_OK)
        return reject_protocol();
    if (info.scope == NA_SCOPE_TERMINAL_MANAGER &&
        !valid_terminal_server_endpoint(
            endpoint, NA_SCOPE_TERMINAL_MANAGER, naos::system::TerminalManager::protocol_uuid,
            naos::system::TerminalManager::revision, naos::system::TerminalManager::features))
    {
        return reject_protocol();
    }
    if (info.scope == NA_SCOPE_TERMINAL_MASTER &&
        (!valid_terminal_server_endpoint(
             endpoint, NA_SCOPE_TERMINAL_MASTER, naos::system::TerminalMaster::protocol_uuid,
             naos::system::TerminalMaster::revision, naos::system::TerminalMaster::features) ||
         find_binding(state, endpoint) == nullptr ||
         find_pair(state, find_binding(state, endpoint)->pair_id) == nullptr))
    {
        return reject_protocol();
    }
    if (info.scope == NA_SCOPE_TERMINAL_SLAVE &&
        (!valid_terminal_server_endpoint(endpoint, NA_SCOPE_TERMINAL_SLAVE, naos::system::TerminalSlave::protocol_uuid,
                                         naos::system::TerminalSlave::revision,
                                         naos::system::TerminalSlave::features) ||
         find_binding(state, endpoint) == nullptr ||
         find_pair(state, find_binding(state, endpoint)->pair_id) == nullptr))
    {
        return reject_protocol();
    }
    auto transport = make_transport();
    const auto responder = transport.responder();
    const uint64_t response_resource_count = response_resource_count_for(info.scope, frame.method_id);
    for (uint64_t i = 0; i < response_resource_count && i < NA_CHANNEL_MAX_RESOURCES; i++)
        response_resources[i] = {};

    if (info.scope == NA_SCOPE_TERMINAL_MANAGER)
    {
        terminal_manager_handler handler(state);
        return naos::system::TerminalManager::server::dispatch(
            handler, responder, endpoint, frame.responder, frame.method_id, request_bytes, frame.actual_bytes,
            request_resources, frame.actual_resources, response_bytes, NA_CHANNEL_MAX_MESSAGE_BYTES, response_resources,
            response_resource_count, frame.caller_pid);
    }
    if (info.scope == NA_SCOPE_TERMINAL_MASTER)
    {
        terminal_master_handler handler(state, endpoint);
        return naos::system::TerminalMaster::server::dispatch(
            handler, responder, endpoint, frame.responder, frame.method_id, request_bytes, frame.actual_bytes,
            request_resources, frame.actual_resources, response_bytes, NA_CHANNEL_MAX_MESSAGE_BYTES, response_resources,
            response_resource_count, frame.caller_pid);
    }
    if (info.scope == NA_SCOPE_TERMINAL_SLAVE)
    {
        terminal_slave_handler handler(state, endpoint);
        return naos::system::TerminalSlave::server::dispatch(
            handler, responder, endpoint, frame.responder, frame.method_id, request_bytes, frame.actual_bytes,
            request_resources, frame.actual_resources, response_bytes, NA_CHANNEL_MAX_MESSAGE_BYTES, response_resources,
            response_resource_count, frame.caller_pid);
    }
    return reject_protocol();
}
} // namespace

namespace
{
service_state g_service_state;
} // namespace

int main()
{
    _s_log("ttyd: starting\n");

    na_handle_t manager_descriptor = NA_HANDLE_INVALID;
    if (_na_protocol_descriptor_create(&naos::system::TerminalManager::descriptor, &manager_descriptor) != NA_STATUS_OK)
    {
        std::printf("ttyd: cannot create TerminalManager descriptor\n");
        _s_log("ttyd: TerminalManager descriptor creation failed\n");
        return 1;
    }
    na_handle_t master_descriptor = NA_HANDLE_INVALID;
    na_handle_t slave_descriptor = NA_HANDLE_INVALID;
    if (_na_protocol_descriptor_create(&naos::system::TerminalMaster::descriptor, &master_descriptor) != NA_STATUS_OK)
    {
        (void)naos_handle_close(manager_descriptor);
        std::printf("ttyd: cannot create terminal descriptors\n");
        _s_log("ttyd: TerminalMaster descriptor creation failed\n");
        return 1;
    }
    if (_na_protocol_descriptor_create(&naos::system::TerminalSlave::descriptor, &slave_descriptor) != NA_STATUS_OK)
    {
        (void)naos_handle_close(master_descriptor);
        (void)naos_handle_close(manager_descriptor);
        std::printf("ttyd: cannot create terminal descriptors\n");
        _s_log("ttyd: TerminalSlave descriptor creation failed\n");
        return 1;
    }

    service_state &state = g_service_state;
    nao::event_loop loop;
    state.master_descriptor = master_descriptor;
    state.slave_descriptor = slave_descriptor;
    const int factory_error = naos_take_terminal_driver_factory(&state.factory_handle);
    if (factory_error != 0)
    {
        (void)naos_handle_close(slave_descriptor);
        (void)naos_handle_close(master_descriptor);
        std::printf("ttyd: terminal factory bootstrap missing (%d)\n", factory_error);
        _s_log("ttyd: factory bootstrap missing\n");
        return 1;
    }
    na_handle_t provider_endpoint = NA_HANDLE_INVALID;
    na_handle_t listener_peer = NA_HANDLE_INVALID;
    if (_na_channel_create(nullptr, &provider_endpoint, &listener_peer) != NA_STATUS_OK)
    {
        (void)naos_handle_close(manager_descriptor);
        std::printf("ttyd: cannot create listener channel\n");
        return 1;
    }
    const int listen_error = naos_service_listen("naos://system/terminal", listener_peer, manager_descriptor, 64);
    if (listen_error != 0)
    {
        (void)naos_handle_close(provider_endpoint);
        (void)naos_handle_close(listener_peer);
        (void)naos_handle_close(manager_descriptor);
        std::printf("ttyd: listener registration failed (%d)\n", listen_error);
        return 1;
    }

    state.listener_endpoint = provider_endpoint;
    state.endpoints[state.endpoint_count++] = provider_endpoint;
    std::printf("ttyd: listener registered; waiting for clients\n");
    _s_log("ttyd: listener registered\n");

    _s_log("ttyd: factory ready\n");

    static na_wait_item_t wait_items[max_wait_items]{};
    auto *request_bytes = static_cast<std::uint8_t *>(malloc(NA_CHANNEL_MAX_MESSAGE_BYTES));
    auto *response_bytes = static_cast<std::uint8_t *>(malloc(NA_CHANNEL_MAX_MESSAGE_BYTES));
    auto *request_resources = static_cast<na_handle_t *>(malloc(sizeof(na_handle_t) * NA_CHANNEL_MAX_RESOURCES));
    if (request_bytes == nullptr || response_bytes == nullptr || request_resources == nullptr)
    {
        _s_log("ttyd: buffer allocation failed\n");
        return 1;
    }

    for (;;)
    {
        flush_pending_creates(state);
        flush_pending_locator_opens(state);
        flush_driver_actions(state);
        uint64_t wait_count = 0;
        for (uint64_t i = 0; i < state.endpoint_count;)
        {
            na_handle_info_t info{};
            info.struct_size = sizeof(info);
            if (_na_handle_get_info(state.endpoints[i], &info) == NA_STATUS_INVALID_HANDLE)
            {
                remove_endpoint(state, state.endpoints[i]);
                continue;
            }
            const na_signal_t endpoint_signals = endpoint_has_pending_write(state, state.endpoints[i])
                                                     ? NA_SIGNAL_PEER_CLOSED
                                                     : NA_SIGNAL_READABLE | NA_SIGNAL_PEER_CLOSED;
            wait_items[wait_count++] = {state.endpoints[i], endpoint_signals, 0};
            i++;
        }
        for (uint64_t i = 0; i < state.pair_count && wait_count < max_wait_items; i++)
        {
            const auto invocation = state.pairs[i].active_driver_invocation;
            if (state.pairs[i].allocated && invocation != NA_HANDLE_INVALID)
                wait_items[wait_count++] = {invocation, NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED, 0};
        }
        for (uint64_t i = 0; i < max_pairs && wait_count < max_wait_items; i++)
        {
            if (state.pending_creates[i].active)
                wait_items[wait_count++] = {state.pending_creates[i].invocation,
                                            NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED, 0};
        }
        for (uint64_t i = 0; i < max_pairs && wait_count < max_wait_items; i++)
        {
            if (state.pending_locator_opens[i].active)
                wait_items[wait_count++] = {state.pending_locator_opens[i].invocation,
                                            NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED, 0};
        }
        const auto append_responder_wait = [&](na_handle_t responder) {
            if (responder != NA_HANDLE_INVALID && wait_count < max_wait_items)
                wait_items[wait_count++] = {responder, NA_SIGNAL_CANCEL_REQUESTED | NA_SIGNAL_PEER_CLOSED, 0};
        };
        for (uint64_t i = 0; i < state.pending_read_count; i++)
            append_responder_wait(state.pending_reads[i].responder);
        for (uint64_t i = 0; i < state.pending_write_count; i++)
            append_responder_wait(state.pending_writes[i].responder);
        for (uint64_t i = 0; i < state.pending_watch_count; i++)
            append_responder_wait(state.pending_watches[i].responder);
        for (uint64_t i = 0; i < max_pairs; i++)
        {
            if (state.pending_creates[i].active)
                append_responder_wait(state.pending_creates[i].responder);
            if (state.pending_locator_opens[i].active)
                append_responder_wait(state.pending_locator_opens[i].responder);
        }
        for (uint64_t i = 0; i < state.pair_count; i++)
        {
            const auto &pair = state.pairs[i];
            if (!pair.allocated)
                continue;
            if (pair.pending_attribute_change.active)
                append_responder_wait(pair.pending_attribute_change.responder);
            append_responder_wait(pair.active_driver_responder);
            for (uint64_t action_index = 0; action_index < pair.pending_driver_action_count; action_index++)
                append_responder_wait(pair.pending_driver_actions[action_index].responder);
        }
        if (wait_count == 0)
            break;
        struct timespec deadline{};
        const auto wait_status =
            loop.wait(wait_items, wait_count, next_read_deadline(state, deadline) ? &deadline : nullptr);
        if (wait_status != NA_STATUS_OK && wait_status != NA_STATUS_WAIT_TIMED_OUT)
            continue;

        for (uint64_t i = 0; i < wait_count; i++)
        {
            if (is_pending_responder(state, wait_items[i].handle))
                continue;
            if ((wait_items[i].observed & (NA_SIGNAL_READABLE | NA_SIGNAL_PEER_CLOSED)) == 0)
            {
                // Driver invocations are harvested by flush_driver_actions;
                // they are included in wait_items only to wake the loop.
                if ((wait_items[i].observed & (NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED)) != 0)
                    continue;
                continue;
            }
            const auto endpoint = wait_items[i].handle;

            bool is_driver_invocation = false;
            bool is_pending_create = false;
            bool is_pending_locator_open = false;
            for (uint64_t pair_index = 0; pair_index < state.pair_count; pair_index++)
            {
                if (state.pairs[pair_index].active_driver_invocation == endpoint)
                {
                    is_driver_invocation = true;
                    break;
                }
            }
            for (uint64_t pending_index = 0; pending_index < max_pairs; pending_index++)
            {
                if (state.pending_creates[pending_index].active &&
                    state.pending_creates[pending_index].invocation == endpoint)
                {
                    is_pending_create = true;
                    break;
                }
            }
            for (uint64_t pending_index = 0; pending_index < max_pairs; pending_index++)
            {
                if (state.pending_locator_opens[pending_index].active &&
                    state.pending_locator_opens[pending_index].invocation == endpoint)
                {
                    is_pending_locator_open = true;
                    break;
                }
            }
            if (is_driver_invocation || is_pending_create || is_pending_locator_open)
                continue;

            if (endpoint == state.listener_endpoint)
            {
                if ((wait_items[i].observed & NA_SIGNAL_PEER_CLOSED) != 0)
                {
                    _s_log("ttyd: listener peer closed; exiting for supervision\n");
                    (void)naos_handle_close(endpoint);
                    state.listener_endpoint = NA_HANDLE_INVALID;
                    return 1;
                }
                na_channel_receive_frame_t frame{};
                frame.struct_size = sizeof(frame);
                frame.byte_capacity = NA_CHANNEL_MAX_MESSAGE_BYTES;
                frame.resource_capacity = NA_CHANNEL_MAX_RESOURCES;
                frame.bytes = reinterpret_cast<std::uint64_t>(request_bytes);
                frame.resources = reinterpret_cast<std::uint64_t>(request_resources);
                const auto receive_status = _na_channel_receive(endpoint, &frame);
                if (receive_status != NA_STATUS_OK)
                {
                    close_received_resources(frame, request_resources);
                    continue;
                }
                if (frame.actual_bytes != 0 || frame.actual_resources != 1 || frame.responder != NA_HANDLE_INVALID ||
                    !valid_terminal_manager_server(request_resources[0]))
                {
                    _s_log("ttyd: rejected malformed TerminalManager handshake\n");
                    close_received_resources(frame, request_resources);
                    continue;
                }
                if (state.endpoint_count < max_endpoints)
                {
                    state.endpoints[state.endpoint_count++] = request_resources[0];
                    std::printf("ttyd: accepted TerminalManager connection\n");
                    _s_log("ttyd: accepted manager connection\n");
                }
                else
                    close_received_resources(frame, request_resources);
                continue;
            }

            na_channel_receive_frame_t frame{};
            frame.struct_size = sizeof(frame);
            frame.byte_capacity = NA_CHANNEL_MAX_MESSAGE_BYTES;
            frame.resource_capacity = NA_CHANNEL_MAX_RESOURCES;
            frame.bytes = reinterpret_cast<std::uint64_t>(request_bytes);
            frame.resources = reinterpret_cast<std::uint64_t>(request_resources);
            const auto receive_status = _na_channel_receive(endpoint, &frame);
            if (receive_status == NA_STATUS_PEER_CLOSED)
            {
                (void)naos_handle_close(endpoint);
                remove_endpoint(state, endpoint);
                continue;
            }
            if (receive_status != NA_STATUS_OK)
                continue;

            na_resource_disposition_t response_resources[NA_CHANNEL_MAX_RESOURCES]{};
            const auto dispatch_status = dispatch_one(state, endpoint, frame, request_bytes, request_resources,
                                                      response_bytes, response_resources);
            // Request resources are installed in ttyd's table for the
            // duration of dispatch.  They are never response-owned.
            close_received_resources(frame, request_resources);
            if (dispatch_status != NA_STATUS_OK)
            {
                if (dispatch_status != NA_STATUS_WOULD_BLOCK)
                {
                    drop_pending_reads(state, endpoint);
                    drop_pending_watches(state, endpoint);
                    (void)naos_handle_close(endpoint);
                    remove_endpoint(state, endpoint);
                }
                if (dispatch_status != NA_STATUS_WOULD_BLOCK)
                {
                    char message[96]{};
                    snprintf(message, sizeof(message), "ttyd: dispatch failed status=%d\n",
                             static_cast<int>(dispatch_status));
                    _s_log(message);
                }
            }
        }
        flush_driver_actions(state);
        flush_pending_creates(state);
        flush_pending_locator_opens(state);
        flush_pending_reads(state);
        flush_pending_writes(state);
        flush_pending_attributes(state);
        flush_pending_watches(state);
    }

    (void)naos_handle_close(provider_endpoint);
    return 0;
}
