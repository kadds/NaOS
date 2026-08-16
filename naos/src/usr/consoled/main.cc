#include "vga_font.hpp"

#include <abi-bits/ioctls.h>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <naos/abi.h>
#include <naos/generated/system/InputEventSource.hpp>
#include <naos/generated/system/InputEventSource_client.hpp>
#include <naos/generated/system/TerminalManager.hpp>
#include <naos/generated/system/TerminalManager_client.hpp>
#include <naos/generated/system/TerminalMaster.hpp>
#include <naos/generated/system/TerminalMaster_client.hpp>
#include <naos/libnao.hpp>
#include <naos/outcome.hpp>
#include <naos/service_directory.hpp>
#include <naos/syscall.h>
#include <vterm.h>

[[gnu::weak]] void *__dso_handle;

extern "C" int naos_take_input_event_source(na_handle_t *handle);
extern "C" int naos_take_console_frontend(na_handle_t *handle);
extern "C" int ioctl(int fd, unsigned long request, ...);

namespace
{
std::uint64_t monotonic_millis()
{
    struct timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * 1000 + static_cast<std::uint64_t>(now.tv_nsec) / 1'000'000;
}

na_handle_t console_frontend = NA_HANDLE_INVALID;
na_handle_t input_event_source = NA_HANDLE_INVALID;

naoidl::native_transport make_transport()
{
    naoidl::native_transport_api api{};
    api.handle_close = [](void *, na_handle_t handle) { return static_cast<na_status_t>(_na_handle_close(handle)); };
    api.handle_get_info = [](void *, na_handle_t handle, na_handle_info_t *info) {
        return static_cast<na_status_t>(_na_handle_get_info(handle, info));
    };
    api.invoke_submit = [](void *, na_handle_t target, const na_submit_frame_t *frame, na_handle_t *invocation) {
        return static_cast<na_status_t>(_na_invoke_submit(target, frame, invocation));
    };
    api.invocation_take_result = [](void *, na_handle_t invocation, na_result_frame_t *frame) {
        return static_cast<na_status_t>(_na_invocation_take_result(invocation, frame));
    };
    return naoidl::native_transport(api);
}

int wait_invocation(na_handle_t invocation)
{
    na_wait_item_t item{invocation, NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED, 0};
    const auto status = _na_handle_wait_many(&item, 1, nullptr);
    return status == NA_STATUS_OK ? 0 : static_cast<int>(status);
}

int invocation_error(const na_result_frame_t &result) { return naos::result_errno(result); }

void close_result_resources(na_handle_t *resources, std::uint64_t count)
{
    if (resources == nullptr)
        return;
    count = count > NA_CHANNEL_MAX_RESOURCES ? NA_CHANNEL_MAX_RESOURCES : count;
    for (std::uint64_t i = 0; i < count; i++)
    {
        if (resources[i] != NA_HANDLE_INVALID)
            (void)naos_handle_close(resources[i]);
        resources[i] = NA_HANDLE_INVALID;
    }
}

int connect_master(na_handle_t &master)
{
    na_handle_t manager = NA_HANDLE_INVALID;
    int error = naos_service_connect_versioned("naos://system/terminal", &naos::system::TerminalManager::protocol_uuid,
                                               NA_PROTOCOL_RIGHT_INVOKE, naos::system::TerminalManager::revision,
                                               naos::system::TerminalManager::features, &manager);
    if (error != 0)
        return error;

    auto transport = make_transport();
    auto client = naos::system::TerminalManager::TerminalManagerClient(transport.async(), manager);
    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    naos::system::TerminalManager::open_console_master_request request{};
    request.mode = 1 | 2;
    if (console_frontend == NA_HANDLE_INVALID &&
        (naos_take_console_frontend(&console_frontend) != 0 || console_frontend == NA_HANDLE_INVALID))
    {
        (void)naos_handle_close(manager);
        return EACCES;
    }
    const na_handle_t frontend = console_frontend;
    request.frontend.value = 0;
    na_resource_disposition_t frontend_resource{};
    frontend_resource.handle = frontend;
    frontend_resource.operation = NA_RESOURCE_DUPLICATE;
    frontend_resource.rights = NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT;
    frontend_resource.scope = NA_SCOPE_NONE;

    na_handle_t invocation = NA_HANDLE_INVALID;
    auto status = client.submit_open_console_master(request, &frontend_resource, 1, &invocation, wire, sizeof(wire));
    if (status != NA_STATUS_OK)
    {
        (void)naos_handle_close(manager);
        return static_cast<int>(status);
    }
    error = wait_invocation(invocation);
    if (error != 0)
    {
        (void)_na_invocation_cancel(invocation);
        (void)naos_handle_close(invocation);
        (void)naos_handle_close(manager);
        return error;
    }
    naos::system::TerminalManager::open_console_master_response response{};
    na_handle_t resources[NA_CHANNEL_MAX_RESOURCES]{};
    na_result_frame_t result{};
    status = client.take_open_console_master(invocation, response, wire, sizeof(wire), resources,
                                             NA_CHANNEL_MAX_RESOURCES, result);
    (void)naos_handle_close(invocation);
    (void)naos_handle_close(manager);
    if (status != NA_STATUS_OK)
    {
        close_result_resources(resources, result.actual_resources);
        return static_cast<int>(status);
    }
    if (const int result_error = invocation_error(result); result_error != 0)
    {
        close_result_resources(resources, result.actual_resources);
        return result_error;
    }
    if (result.actual_resources != 2)
    {
        close_result_resources(resources, result.actual_resources);
        return EIO;
    }
    if (response.master.value >= result.actual_resources || response.job_control.value >= result.actual_resources ||
        response.master.value == response.job_control.value)
    {
        close_result_resources(resources, result.actual_resources);
        return EIO;
    }
    master = resources[response.master.value];
    resources[response.master.value] = NA_HANDLE_INVALID;
    if (response.job_control.value < result.actual_resources)
        (void)naos_handle_close(resources[response.job_control.value]);
    resources[response.job_control.value] = NA_HANDLE_INVALID;
    close_result_resources(resources, result.actual_resources);
    return 0;
}

enum class master_async_kind : std::uint8_t
{
    none,
    read,
    read_watch,
    write,
    write_watch,
};

struct master_async_request
{
    master_async_kind kind = master_async_kind::none;
    na_handle_t invocation = NA_HANDLE_INVALID;
    std::uint8_t wire[4096]{};
    std::size_t requested = 0;
    std::uint64_t generation = 0;
};

constexpr std::uint64_t terminal_readable = 0x001;
constexpr std::uint64_t terminal_writable = 0x004;
constexpr std::uint64_t terminal_hangup = 0x010;

void close_master_async_request(master_async_request &request)
{
    if (request.invocation != NA_HANDLE_INVALID)
        (void)naos_handle_close(request.invocation);
    request = {};
}

int submit_master_read(na_handle_t master, master_async_request &request)
{
    if (request.kind != master_async_kind::none)
        return EBUSY;
    auto transport = make_transport();
    auto client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master);
    naos::system::TerminalMaster::read_request message{};
    message.size = 1024;
    // Let ttyd retain this invocation while output is unavailable.  A
    // non-blocking EAGAIN is represented as a failed RPC by the generic
    // server dispatcher, which would close the terminal endpoint.
    message.flags = 0;
    const auto status =
        client.submit_read(message, nullptr, 0, &request.invocation, request.wire, sizeof(request.wire));
    if (status != NA_STATUS_OK)
    {
        request.invocation = NA_HANDLE_INVALID;
        return EIO;
    }
    request.kind = master_async_kind::read;
    request.requested = message.size;
    return 0;
}

int submit_master_watch(na_handle_t master, master_async_request &request, std::uint64_t mask)
{
    if (request.kind != master_async_kind::none)
        return EBUSY;
    auto transport = make_transport();
    auto client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master);
    naos::system::TerminalMaster::watch_request message{};
    message.mask = mask;
    message.observed_generation = request.generation;
    const auto status =
        client.submit_watch(message, nullptr, 0, &request.invocation, request.wire, sizeof(request.wire));
    if (status != NA_STATUS_OK)
    {
        request.invocation = NA_HANDLE_INVALID;
        return EIO;
    }
    request.kind = mask == terminal_writable ? master_async_kind::write_watch : master_async_kind::read_watch;
    return 0;
}

int submit_master_write(na_handle_t master, master_async_request &request, const std::uint8_t *data, std::size_t size)
{
    if (request.kind != master_async_kind::none || size == 0 || size > sizeof(request.wire) / 2)
        return EINVAL;
    auto transport = make_transport();
    auto client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master);
    naos::system::TerminalMaster::write_request message{};
    message.size = size;
    // A full terminal input queue is represented by a pending async write;
    // using non-blocking EAGAIN here would make the server dispatcher close
    // the otherwise healthy endpoint.
    message.flags = 0;
    message.data = {data, static_cast<std::uint32_t>(size)};
    const auto status =
        client.submit_write(message, nullptr, 0, &request.invocation, request.wire, sizeof(request.wire));
    if (status != NA_STATUS_OK)
    {
        if (status == NA_STATUS_WOULD_BLOCK)
        {
            request.invocation = NA_HANDLE_INVALID;
            return EAGAIN;
        }
        switch (status)
        {
            case NA_STATUS_ACCESS_DENIED:
                _s_log("consoled: master write access denied\n");
                break;
            case NA_STATUS_INVALID_MESSAGE:
                _s_log("consoled: master write invalid message\n");
                break;
            case NA_STATUS_INVALID_HANDLE:
                _s_log("consoled: master write invalid handle\n");
                break;
            default: {
                char message[96]{};
                snprintf(message, sizeof(message), "consoled: master write submit status=%d\n",
                         static_cast<int>(status));
                _s_log(message);
                break;
            }
        }
        request.invocation = NA_HANDLE_INVALID;
        return EIO;
    }
    request.kind = master_async_kind::write;
    request.requested = size;
    return 0;
}

int complete_master_read(na_handle_t master, master_async_request &request, std::uint8_t *buffer, std::size_t capacity,
                         std::size_t &read, bool &hangup, bool &readable)
{
    read = 0;
    hangup = false;
    readable = false;
    auto transport = make_transport();
    na_result_frame_t result{};
    if (request.kind == master_async_kind::read_watch)
    {
        naos::system::TerminalMaster::watch_response response{};
        auto client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master);
        const auto status =
            client.take_watch(request.invocation, response, request.wire, sizeof(request.wire), nullptr, 0, result);
        const auto generation = response.readiness.generation;
        close_master_async_request(request);
        if (status != NA_STATUS_OK)
            return EIO;
        const int error = invocation_error(result);
        if (error != 0)
            return error;
        hangup = (response.readiness.hangup_mask & terminal_hangup) != 0;
        readable = (response.readiness.ready_mask & terminal_readable) != 0;
        request.generation = generation;
        return 0;
    }
    if (request.kind != master_async_kind::read)
        return EINVAL;
    naos::system::TerminalMaster::read_response response{};
    auto client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master);
    const auto status =
        client.take_read(request.invocation, response, request.wire, sizeof(request.wire), nullptr, 0, result);
    if (status != NA_STATUS_OK)
    {
        close_master_async_request(request);
        return EIO;
    }
    const int error = invocation_error(result);
    if (error != 0)
    {
        close_master_async_request(request);
        return error;
    }
    if (response.data.size > capacity)
    {
        close_master_async_request(request);
        return EOVERFLOW;
    }
    // Generated decoders borrow inline bytes from request.wire. Copy them
    // before close_master_async_request() clears that storage.
    std::memcpy(buffer, response.data.data, response.data.size);
    read = response.data.size;
    close_master_async_request(request);
    readable = read != 0;
    return 0;
}

int complete_master_write(na_handle_t master, master_async_request &request, std::size_t &written, bool &would_block)
{
    written = 0;
    would_block = false;
    if (request.kind == master_async_kind::write_watch)
    {
        auto transport = make_transport();
        auto client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master);
        naos::system::TerminalMaster::watch_response response{};
        na_result_frame_t result{};
        const auto status =
            client.take_watch(request.invocation, response, request.wire, sizeof(request.wire), nullptr, 0, result);
        close_master_async_request(request);
        if (status != NA_STATUS_OK)
            return EIO;
        return invocation_error(result);
    }
    if (request.kind != master_async_kind::write)
        return EINVAL;
    const std::size_t requested = request.requested;
    auto transport = make_transport();
    auto client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master);
    naos::system::TerminalMaster::write_response response{};
    na_result_frame_t result{};
    const auto status =
        client.take_write(request.invocation, response, request.wire, sizeof(request.wire), nullptr, 0, result);
    close_master_async_request(request);
    if (status != NA_STATUS_OK)
        return EIO;
    const int error = invocation_error(result);
    if (error == EAGAIN)
    {
        would_block = true;
        return 0;
    }
    if (error != 0)
        return error;
    if (response.count > requested)
        return EOVERFLOW;
    written = response.count;
    return 0;
}

int master_set_winsize(na_handle_t master, int rows, int cols, std::uint32_t width, std::uint32_t height)
{
    auto transport = make_transport();
    auto client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master);
    std::uint8_t wire[256]{};
    naos::system::TerminalMaster::set_winsize_request request{};
    request.size.rows = static_cast<std::uint16_t>(rows);
    request.size.columns = static_cast<std::uint16_t>(cols);
    request.size.x_pixels = static_cast<std::uint16_t>(width);
    request.size.y_pixels = static_cast<std::uint16_t>(height);
    na_handle_t invocation = NA_HANDLE_INVALID;
    auto status = client.submit_set_winsize(request, nullptr, 0, &invocation, wire, sizeof(wire));
    if (status != NA_STATUS_OK)
        return static_cast<int>(status);
    const int error = wait_invocation(invocation);
    if (error != 0)
    {
        (void)naos_handle_close(invocation);
        return error;
    }
    naos::system::TerminalMaster::set_winsize_response response{};
    na_result_frame_t result{};
    status = client.take_set_winsize(invocation, response, wire, sizeof(wire), nullptr, 0, result);
    (void)naos_handle_close(invocation);
    if (status != NA_STATUS_OK)
        return static_cast<int>(status);
    return invocation_error(result);
}

int subscribe_input(na_handle_t &receiver)
{
    if (input_event_source == NA_HANDLE_INVALID)
    {
        const int error = naos_take_input_event_source(&input_event_source);
        if (error != 0)
            return error;
    }

    auto transport = make_transport();
    auto client = naos::system::InputEventSource::InputEventSourceClient(transport.async(), input_event_source);
    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    naos::system::InputEventSource::subscribe_request request{};
    request.max_events = 64;
    na_handle_t invocation = NA_HANDLE_INVALID;
    auto status = client.submit_subscribe(request, nullptr, 0, &invocation, wire, sizeof(wire));
    if (status != NA_STATUS_OK)
        return static_cast<int>(status);

    const int error = wait_invocation(invocation);
    if (error != 0)
    {
        (void)_na_invocation_cancel(invocation);
        (void)naos_handle_close(invocation);
        return error;
    }
    naos::system::InputEventSource::subscribe_response response{};
    na_handle_t resources[NA_CHANNEL_MAX_RESOURCES]{};
    na_result_frame_t result{};
    status =
        client.take_subscribe(invocation, response, wire, sizeof(wire), resources, NA_CHANNEL_MAX_RESOURCES, result);
    (void)naos_handle_close(invocation);
    if (status != NA_STATUS_OK)
    {
        close_result_resources(resources, result.actual_resources);
        return static_cast<int>(status);
    }
    if (invocation_error(result) != 0 || result.actual_resources != 1 ||
        response.receiver.value >= result.actual_resources)
    {
        close_result_resources(resources, result.actual_resources);
        return EIO;
    }
    receiver = resources[response.receiver.value];
    resources[response.receiver.value] = NA_HANDLE_INVALID;
    return 0;
}

enum class console_key : std::uint64_t
{
    escape = 0x1,
    n1,
    n2,
    n3,
    n4,
    n5,
    n6,
    n7,
    n8,
    n9,
    n0,
    minus,
    equal,
    backspace,
    tab,
    q,
    w,
    e,
    r,
    t,
    y,
    u,
    i,
    o,
    p,
    left_brackets,
    right_brackets,
    enter,
    left_control,
    a,
    s,
    d,
    f,
    g,
    h,
    j,
    k,
    l,
    semicolon,
    quote,
    back_tick,
    left_shift,
    backslash,
    z,
    x,
    c,
    v,
    b,
    n,
    m,
    comma,
    period,
    slash,
    right_shift,
    pad_mul,
    left_alt,
    space,
    capslock,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    numlock,
    scrolllock,
    pad_7,
    pad_8,
    pad_9,
    pad_minus,
    pad_4,
    pad_5,
    pad_6,
    pad_plus,
    pad_1,
    pad_2,
    pad_3,
    pad_0,
    pad_comma,
    f11 = 0x57,
    f12 = 0x58,
    pad_enter = 0x6c,
    right_control,
    mute = 0x70,
    calc,
    play,
    stop = 0x74,
    volume_down = 0x7e,
    volume_up = 0x80,
    pad_slash = 0x85,
    right_alt = 0x88,
    home = 0x97,
    cur_up,
    page_up,
    cur_left,
    cur_right = 0x9d,
    end = 0x9f,
    cur_down,
    page_down,
    insert,
    delete_key,
    print = 0xfd,
    pause = 0xfe,
};

const char console_key_char_table[256] = {
    0,   0,   '1', '2', '3', '4', '5',  '6', '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e',  'r', 't',  'y',
    'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's', 'd', 'f', 'g', 'h', 'j',  'k',  'l', ';', '\'', '`', 0,    '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm',  ',', '.', '/', 0,   '*', 0,   ' ', 0,    0,    0,   0,   0,    0,   0,    0,
    0,   0,   0,   0,   0,   '7', '8',  '9', '-', '4', '5', '6', '+', '1', '2',  '3',  '0', '.', 0,    0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   0,   0,   0,   0,    0,    0,   0,   0,    0,   '\n', 0};

const char console_key_char_table2[256] = {
    0,   0,   '!', '@', '#', '$', '%',  '^', '&', '*', '(', ')', '_', '+', 0,   0,   'Q', 'W', 'E', 'R', 'T',  'Y',
    'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,    '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M',  '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,    0,
    0,   0,   0,   0,   0,   '7', '8',  '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', 0,   0,   0,    0,
    0,   0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '\n', 0};

bool console_capslock = false;
bool console_numlock = false;
bool console_scrolllock = false;
bool console_compose_pending = false;
bool console_pressed[256]{};
std::uint8_t console_modifier_state = 0;

bool is_modifier_key(console_key key, std::uint8_t &mask)
{
    switch (key)
    {
        case console_key::left_control:
        case console_key::right_control:
            mask = 1;
            return true;
        case console_key::left_alt:
        case console_key::right_alt:
            mask = 2;
            return true;
        case console_key::left_shift:
        case console_key::right_shift:
            mask = 4;
            return true;
        default:
            mask = 0;
            return false;
    }
}

VTermModifier key_modifiers(const naos::system::InputEventSource::KeyEvent &event)
{
    VTermModifier modifiers = VTERM_MOD_NONE;
    const auto event_modifiers = static_cast<std::uint8_t>(event.modifiers) | console_modifier_state;
    if ((event_modifiers & 1) != 0)
        modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_CTRL);
    if ((event_modifiers & 2) != 0)
        modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_ALT);
    if ((event_modifiers & 4) != 0)
        modifiers = static_cast<VTermModifier>(modifiers | VTERM_MOD_SHIFT);
    return modifiers;
}

bool key_to_vterm(console_key key, VTermKey &result)
{
    switch (key)
    {
        case console_key::enter:
        case console_key::pad_enter:
            result = VTERM_KEY_ENTER;
            return true;
        case console_key::tab:
            result = VTERM_KEY_TAB;
            return true;
        case console_key::backspace:
            result = VTERM_KEY_BACKSPACE;
            return true;
        case console_key::escape:
            result = VTERM_KEY_ESCAPE;
            return true;
        case console_key::cur_up:
            result = VTERM_KEY_UP;
            return true;
        case console_key::cur_down:
            result = VTERM_KEY_DOWN;
            return true;
        case console_key::cur_left:
            result = VTERM_KEY_LEFT;
            return true;
        case console_key::cur_right:
            result = VTERM_KEY_RIGHT;
            return true;
        case console_key::home:
            result = VTERM_KEY_HOME;
            return true;
        case console_key::end:
            result = VTERM_KEY_END;
            return true;
        case console_key::insert:
            result = VTERM_KEY_INS;
            return true;
        case console_key::delete_key:
            result = VTERM_KEY_DEL;
            return true;
        case console_key::page_up:
            result = VTERM_KEY_PAGEUP;
            return true;
        case console_key::page_down:
            result = VTERM_KEY_PAGEDOWN;
            return true;
        default:
            return false;
    }
}

void emit_key_event(VTerm *vt, const naos::system::InputEventSource::KeyEvent &event)
{
    const auto k = static_cast<console_key>(event.key_code);
    if (event.kind != NA_INPUT_EVENT_KIND_REPEAT && k == console_key::capslock)
    {
        console_capslock = !console_capslock;
        return;
    }
    if (event.kind != NA_INPUT_EVENT_KIND_REPEAT && k == console_key::numlock)
    {
        console_numlock = !console_numlock;
        return;
    }
    if (event.kind != NA_INPUT_EVENT_KIND_REPEAT && k == console_key::scrolllock)
    {
        console_scrolllock = !console_scrolllock;
        return;
    }
    const bool ctrl = (event.modifiers & 1) != 0;
    const bool alt = (event.modifiers & 2) != 0;
    const bool shift = (event.modifiers & 4) != 0;
    if (ctrl && alt && k == console_key::f12)
        return;

    const auto modifiers = key_modifiers(event);
    VTermKey key = VTERM_KEY_NONE;
    if (!console_numlock)
    {
        switch (k)
        {
            case console_key::pad_8:
                key = VTERM_KEY_UP;
                break;
            case console_key::pad_2:
                key = VTERM_KEY_DOWN;
                break;
            case console_key::pad_4:
                key = VTERM_KEY_LEFT;
                break;
            case console_key::pad_6:
                key = VTERM_KEY_RIGHT;
                break;
            case console_key::pad_7:
                key = VTERM_KEY_HOME;
                break;
            case console_key::pad_1:
                key = VTERM_KEY_END;
                break;
            case console_key::pad_9:
                key = VTERM_KEY_PAGEUP;
                break;
            case console_key::pad_3:
                key = VTERM_KEY_PAGEDOWN;
                break;
            default:
                break;
        }
        if (key != VTERM_KEY_NONE)
        {
            vterm_keyboard_key(vt, key, modifiers);
            return;
        }
    }
    if (key_to_vterm(k, key))
    {
        vterm_keyboard_key(vt, key, modifiers);
        return;
    }
    if (k >= console_key::f1 && k <= console_key::f12)
    {
        const auto index = static_cast<int>(k) - static_cast<int>(console_key::f1) + 1;
        vterm_keyboard_key(vt, static_cast<VTermKey>(VTERM_KEY_FUNCTION(index)), modifiers);
        return;
    }

    std::uint32_t d = static_cast<unsigned char>(console_key_char_table[static_cast<unsigned>(event.key_code) & 0xff]);
    if (shift)
        d = static_cast<unsigned char>(console_key_char_table2[static_cast<unsigned>(event.key_code) & 0xff]);
    if (console_capslock)
    {
        if (d >= 'a' && d <= 'z')
            d = d - 'a' + 'A';
        else if (d >= 'A' && d <= 'Z')
            d = d - 'A' + 'a';
    }
    if (d == 0)
        return;
    if (k == console_key::back_tick && !ctrl && !alt)
    {
        console_compose_pending = true;
        return;
    }
    if (console_compose_pending)
    {
        console_compose_pending = false;
        if (d == 'a')
            d = 0x00e0; // grave-accent dead key + a
    }
    vterm_keyboard_unichar(vt, d, modifiers);
}

std::uint32_t color_to_u32(const VTermColor &color)
{
    return static_cast<std::uint32_t>(color.rgb.red) << 16 | static_cast<std::uint32_t>(color.rgb.green) << 8 |
           static_cast<std::uint32_t>(color.rgb.blue);
}

struct scrollback_store
{
    static constexpr int capacity = 256;
    static constexpr int max_columns = 512;
    struct line
    {
        VTermScreenCell cells[max_columns]{};
    };

    line lines[capacity]{};
    int head = 0;
    int size = 0;

    int push(int columns, const VTermScreenCell *cells)
    {
        if (cells == nullptr || columns < 0 || columns > max_columns)
            return 0;
        const int index = (head + size) % capacity;
        std::memset(lines[index].cells, 0, sizeof(lines[index].cells));
        std::memcpy(lines[index].cells, cells, static_cast<std::size_t>(columns) * sizeof(VTermScreenCell));
        if (size == capacity)
            head = (head + 1) % capacity;
        else
            size++;
        return 1;
    }

    int pop(int columns, VTermScreenCell *cells)
    {
        if (cells == nullptr || columns < 0 || columns > max_columns)
            return 0;
        if (size == 0)
        {
            std::memset(cells, 0, static_cast<std::size_t>(columns) * sizeof(VTermScreenCell));
            return 0;
        }
        const int index = (head + size - 1) % capacity;
        std::memcpy(cells, lines[index].cells, static_cast<std::size_t>(columns) * sizeof(VTermScreenCell));
        size--;
        return 1;
    }

    int clear()
    {
        head = 0;
        size = 0;
        return 1;
    }
};

struct renderer
{
    int rows = 0;
    int cols = 0;
    int pitch_pixels = 0;
    std::uint32_t *backbuffer = nullptr;
    bool damaged = false;
    VTermRect damage{};
    bool cursor_visible = false;
    VTermPos cursor{};
    scrollback_store *scrollback = nullptr;
};

void add_damage(renderer &state, VTermRect rect)
{
    rect.start_row = rect.start_row < 0 ? 0 : rect.start_row;
    rect.start_col = rect.start_col < 0 ? 0 : rect.start_col;
    rect.end_row = rect.end_row > state.rows ? state.rows : rect.end_row;
    rect.end_col = rect.end_col > state.cols ? state.cols : rect.end_col;
    if (rect.start_row >= rect.end_row || rect.start_col >= rect.end_col)
        return;
    if (!state.damaged)
    {
        state.damage = rect;
        state.damaged = true;
        return;
    }
    state.damage.start_row = state.damage.start_row < rect.start_row ? state.damage.start_row : rect.start_row;
    state.damage.start_col = state.damage.start_col < rect.start_col ? state.damage.start_col : rect.start_col;
    state.damage.end_row = state.damage.end_row > rect.end_row ? state.damage.end_row : rect.end_row;
    state.damage.end_col = state.damage.end_col > rect.end_col ? state.damage.end_col : rect.end_col;
}

std::uint32_t brighten(std::uint32_t color)
{
    const auto component = [](std::uint32_t value) { return value + (0xffU - value) / 3; };
    return (component((color >> 16) & 0xffU) << 16) | (component((color >> 8) & 0xffU) << 8) | component(color & 0xffU);
}

void render_cell(VTermScreen *screen, renderer &state, int row, int col)
{
    VTermPos pos{};
    VTermScreenCell cell{};
    pos.row = row;
    pos.col = col;
    if (!vterm_screen_get_cell(screen, pos, &cell))
        return;
    vterm_screen_convert_color_to_rgb(screen, &cell.fg);
    vterm_screen_convert_color_to_rgb(screen, &cell.bg);
    std::uint32_t fg = color_to_u32(cell.fg);
    std::uint32_t bg = color_to_u32(cell.bg);
    if (cell.attrs.reverse || (state.cursor_visible && state.cursor.row == row && state.cursor.col == col))
    {
        const auto temp = fg;
        fg = bg;
        bg = temp;
    }
    if (cell.attrs.bold)
        fg = brighten(fg);
    if (cell.attrs.conceal || cell.width == 0)
        fg = bg;

    const std::uint32_t ch = cell.chars[0];
    // vterm uses U+0000 for an empty cell.  Keep it blank instead of sending
    // it through the replacement glyph path, otherwise the whole screen is
    // filled with visible garbage before the first output arrives.
    const std::uint32_t glyph = ch == 0 ? 0 : (ch >= 0x20 && ch <= 0x7e ? ch : 0x7f);
    // vga_font.hpp keeps the source file's codepoint marker before every
    // non-zero glyph.  The marker is not a scanline and must be skipped.
    const std::size_t font_offset = glyph == 0 ? 0 : glyph * 17 + 1;
    for (int y = 0; y < 16; y++)
    {
        const std::uint8_t bits = consoled::vga_font[font_offset + y];
        std::uint32_t *line = state.backbuffer + (row * 16 + y) * state.pitch_pixels + col * 8;
        for (int x = 0; x < 8; x++)
        {
            bool ink = (bits & (0x80 >> x)) != 0;
            if (cell.attrs.underline && y == 15)
                ink = true;
            if (cell.attrs.strike && y == 8)
                ink = true;
            line[x] = ink ? fg : bg;
        }
    }
}

void render_damage(VTermScreen *screen, renderer &state, std::uint32_t *scanout)
{
    if (!state.damaged)
        return;
    const auto rect = state.damage;
    for (int row = rect.start_row; row < rect.end_row; row++)
        for (int col = rect.start_col; col < rect.end_col; col++)
            render_cell(screen, state, row, col);

    const std::size_t start_pixel = static_cast<std::size_t>(rect.start_col) * 8;
    const std::size_t pixels = static_cast<std::size_t>(rect.end_col - rect.start_col) * 8;
    for (int y = rect.start_row * 16; y < rect.end_row * 16; y++)
    {
        auto *source = state.backbuffer + static_cast<std::size_t>(y) * state.pitch_pixels + start_pixel;
        auto *destination = scanout + static_cast<std::size_t>(y) * state.pitch_pixels + start_pixel;
        memcpy(destination, source, pixels * sizeof(std::uint32_t));
    }
    state.damaged = false;
}

int damage_callback(VTermRect rect, void *user)
{
    add_damage(*static_cast<renderer *>(user), rect);
    return 1;
}

int cursor_callback(VTermPos pos, VTermPos old_pos, int visible, void *user)
{
    auto &state = *static_cast<renderer *>(user);
    add_damage(state, {old_pos.row, old_pos.row + 1, old_pos.col, old_pos.col + 1});
    add_damage(state, {pos.row, pos.row + 1, pos.col, pos.col + 1});
    state.cursor = pos;
    state.cursor_visible = visible != 0;
    return 1;
}

int scrollback_pushline(int columns, const VTermScreenCell *cells, void *user)
{
    auto *state = static_cast<renderer *>(user);
    return state->scrollback == nullptr ? 0 : state->scrollback->push(columns, cells);
}

int scrollback_popline(int columns, VTermScreenCell *cells, void *user)
{
    auto *state = static_cast<renderer *>(user);
    return state->scrollback == nullptr ? 0 : state->scrollback->pop(columns, cells);
}

int scrollback_clear(void *user)
{
    auto *state = static_cast<renderer *>(user);
    return state->scrollback == nullptr ? 0 : state->scrollback->clear();
}
} // namespace

int main(int argc, char **argv)
{
    _s_log("consoled: starting\n");
    nao::event_loop loop;
    int master_fd = -1;
    if (argc > 1)
        master_fd = atoi(argv[1]);
    int fb_fd = open("/dev/fb0", O_RDWR | O_EXCL);
    if (fb_fd < 0)
    {
        std::printf("consoled: cannot open /dev/fb0\n");
        char message[96]{};
        snprintf(message, sizeof(message), "consoled: fb open failed errno=%d\n", errno);
        _s_log(message);
        return 1;
    }
    fb_fix_screeninfo fix{};
    fb_var_screeninfo var{};
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) != 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) != 0)
    {
        std::printf("consoled: cannot query framebuffer\n");
        close(fb_fd);
        return 1;
    }
    if (var.bits_per_pixel != 32 || fix.line_length < var.xres * sizeof(std::uint32_t) || var.yres < 16 || var.xres < 8)
    {
        _s_log("consoled: unsupported framebuffer layout\n");
        close(fb_fd);
        return 1;
    }
    const std::uint64_t visible_frame_bytes = static_cast<std::uint64_t>(fix.line_length) * var.yres;
    if (fix.smem_len < visible_frame_bytes)
    {
        _s_log("consoled: framebuffer memory is smaller than visible frame\n");
        close(fb_fd);
        return 1;
    }
    constexpr std::uint64_t page_size = 4096;
    std::uint64_t mapped_bytes =
        (static_cast<std::uint64_t>(fix.smem_len) + page_size - 1) & ~(page_size - 1);
    auto *scanout =
        static_cast<std::uint32_t *>(mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0));
    if (scanout == MAP_FAILED)
    {
        std::printf("consoled: cannot map framebuffer\n");
        _s_log("consoled: mmap failed\n");
        close(fb_fd);
        return 1;
    }
    int rows = static_cast<int>(var.yres / 16);
    int cols = static_cast<int>(var.xres / 8);
    int pitch_pixels = static_cast<int>(fix.line_length / 4);
    auto *backbuffer =
        static_cast<std::uint32_t *>(calloc(static_cast<std::size_t>(rows * 16) * pitch_pixels, sizeof(std::uint32_t)));
    if (backbuffer == nullptr)
    {
        _s_log("consoled: backbuffer alloc failed\n");
        return 1;
    }
    scrollback_store scrollback;
    renderer render_state{rows, cols, pitch_pixels, backbuffer, false, {}, false, {}, &scrollback};
    bool framebuffer_enabled = true;

    VTerm *vt = vterm_new(rows, cols);
    if (vt == nullptr)
    {
        _s_log("consoled: vterm_new failed\n");
        return 1;
    }
    VTermScreen *screen = vterm_obtain_screen(vt);
    VTermScreenCallbacks callbacks{};
    callbacks.damage = damage_callback;
    callbacks.movecursor = cursor_callback;
    callbacks.sb_pushline = scrollback_pushline;
    callbacks.sb_popline = scrollback_popline;
    callbacks.sb_clear = scrollback_clear;
    vterm_screen_set_callbacks(screen, &callbacks, &render_state);
    vterm_screen_set_damage_merge(screen, VTERM_DAMAGE_ROW);
    vterm_screen_reset(screen, 1);
    vterm_screen_enable_altscreen(screen, 1);
    vterm_set_utf8(vt, 1);
    add_damage(render_state, {0, rows, 0, cols});
    render_damage(screen, render_state, scanout);

    na_handle_t master = NA_HANDLE_INVALID;
    if (master_fd < 0)
    {
        int connect_error = EAGAIN;
        for (int attempt = 0; attempt < 8 && connect_error == EAGAIN; attempt++)
        {
            connect_error = connect_master(master);
            if (connect_error == EAGAIN)
                sleep(1);
        }
        if (connect_error != 0)
        {
            char message[96]{};
            snprintf(message, sizeof(message), "consoled: cannot connect ttyd (%d)\n", connect_error);
            std::printf("%s", message);
            _s_log(message);
            return 1;
        }
    }
    else if (fcntl(master_fd, F_SETFL, O_NONBLOCK) != 0)
    {
        char message[96]{};
        snprintf(message, sizeof(message), "consoled: cannot set nonblock errno=%d\n", errno);
        _s_log(message);
        std::printf("consoled: cannot set nonblock errno=%d\n", errno);
        return 1;
    }

    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = static_cast<unsigned short>(var.xres);
    ws.ws_ypixel = static_cast<unsigned short>(var.yres);
    if (master_fd >= 0)
        (void)ioctl(master_fd, TIOCSWINSZ, &ws);
    else
        (void)master_set_winsize(master, rows, cols, var.xres, var.yres);

    na_handle_t input_receiver = NA_HANDLE_INVALID;
    std::uint64_t input_retry_after = 0;
    std::uint64_t input_retry_delay = 100;
    std::uint32_t input_retry_failures = 0;
    int shutdown_status = 1;
    const int input_error = subscribe_input(input_receiver);
    if (input_error != 0)
    {
        char message[96]{};
        snprintf(message, sizeof(message), "consoled: input subscribe failed (%d)\n", input_error);
        _s_log(message);
    }
    else
    {
        _s_log("consoled: input subscribed\n");
    }

    master_async_request master_read_request{};
    master_async_request master_write_request{};
    std::uint8_t pending_master_output[8192]{};
    std::size_t pending_master_output_size = 0;

    auto disable_framebuffer = [&]() {
        if (!framebuffer_enabled)
            return;
        if (scanout != nullptr && scanout != MAP_FAILED)
        {
            (void)munmap(scanout, mapped_bytes);
            scanout = nullptr;
        }
        if (fb_fd >= 0)
        {
            close(fb_fd);
            fb_fd = -1;
        }
        framebuffer_enabled = false;
        _s_log("consoled: framebuffer disabled\n");
    };

    auto enable_framebuffer = [&]() -> bool {
        if (framebuffer_enabled)
            return true;

        const int new_fb_fd = open("/dev/fb0", O_RDWR | O_EXCL);
        if (new_fb_fd < 0)
        {
            _s_log("consoled: framebuffer enable open failed\n");
            return false;
        }
        fb_fix_screeninfo new_fix{};
        fb_var_screeninfo new_var{};
        if (ioctl(new_fb_fd, FBIOGET_FSCREENINFO, &new_fix) != 0 ||
            ioctl(new_fb_fd, FBIOGET_VSCREENINFO, &new_var) != 0 || new_var.bits_per_pixel != 32 ||
            new_fix.line_length < new_var.xres * sizeof(std::uint32_t) || new_var.yres < 16 || new_var.xres < 8)
        {
            close(new_fb_fd);
            _s_log("consoled: framebuffer enable query failed\n");
            return false;
        }

        const std::uint64_t new_visible_frame_bytes =
            static_cast<std::uint64_t>(new_fix.line_length) * new_var.yres;
        if (new_fix.smem_len < new_visible_frame_bytes)
        {
            close(new_fb_fd);
            _s_log("consoled: framebuffer memory is smaller than visible frame\n");
            return false;
        }
        const std::uint64_t new_mapped_bytes =
            (static_cast<std::uint64_t>(new_fix.smem_len) + page_size - 1) & ~(page_size - 1);
        auto *new_scanout = static_cast<std::uint32_t *>(
            mmap(nullptr, new_mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, new_fb_fd, 0));
        if (new_scanout == MAP_FAILED)
        {
            close(new_fb_fd);
            _s_log("consoled: framebuffer enable mmap failed\n");
            return false;
        }

        const int new_rows = static_cast<int>(new_var.yres / 16);
        const int new_cols = static_cast<int>(new_var.xres / 8);
        const int new_pitch_pixels = static_cast<int>(new_fix.line_length / sizeof(std::uint32_t));
        const bool geometry_changed = new_rows != rows || new_cols != cols || new_pitch_pixels != pitch_pixels ||
                                      new_var.xres != var.xres || new_var.yres != var.yres;
        auto *new_backbuffer = backbuffer;
        if (geometry_changed)
        {
            new_backbuffer = static_cast<std::uint32_t *>(
                calloc(static_cast<std::size_t>(new_rows * 16) * new_pitch_pixels, sizeof(std::uint32_t)));
            if (new_backbuffer == nullptr)
            {
                (void)munmap(new_scanout, new_mapped_bytes);
                close(new_fb_fd);
                _s_log("consoled: framebuffer enable backbuffer failed\n");
                return false;
            }
        }

        if (geometry_changed)
        {
            free(backbuffer);
            backbuffer = new_backbuffer;
            render_state.rows = new_rows;
            render_state.cols = new_cols;
            render_state.pitch_pixels = new_pitch_pixels;
            render_state.backbuffer = new_backbuffer;
            vterm_set_size(vt, new_rows, new_cols);
            rows = new_rows;
            cols = new_cols;
            pitch_pixels = new_pitch_pixels;
        }
        fb_fd = new_fb_fd;
        scanout = new_scanout;
        mapped_bytes = new_mapped_bytes;
        fix = new_fix;
        var = new_var;
        framebuffer_enabled = true;

        struct winsize enable_ws{};
        enable_ws.ws_row = static_cast<unsigned short>(rows);
        enable_ws.ws_col = static_cast<unsigned short>(cols);
        enable_ws.ws_xpixel = static_cast<unsigned short>(var.xres);
        enable_ws.ws_ypixel = static_cast<unsigned short>(var.yres);
        if (master_fd >= 0)
            (void)ioctl(master_fd, TIOCSWINSZ, &enable_ws);
        else
            (void)master_set_winsize(master, rows, cols, var.xres, var.yres);

        add_damage(render_state, {0, rows, 0, cols});
        vterm_screen_flush_damage(screen);
        render_damage(screen, render_state, scanout);
        _s_log("consoled: framebuffer enabled\n");
        return true;
    };

    for (;;)
    {
        const auto retry_now = monotonic_millis();
        if (input_receiver == NA_HANDLE_INVALID && retry_now >= input_retry_after)
        {
            na_handle_t replacement = NA_HANDLE_INVALID;
            const int retry_error = subscribe_input(replacement);
            if (retry_error == 0)
            {
                input_receiver = replacement;
                input_retry_delay = 100;
                input_retry_after = 0;
                input_retry_failures = 0;
                _s_log("consoled: input receiver resubscribed\n");
            }
            else
            {
                input_retry_failures++;
                if (input_retry_failures >= 8)
                {
                    _s_log("consoled: input receiver unavailable; shutting down\n");
                    goto shutdown;
                }
                input_retry_after = retry_now + input_retry_delay;
                input_retry_delay = input_retry_delay < 5000 ? input_retry_delay * 2 : 5000;
            }
        }
        struct timespec resize_now{};
        if (framebuffer_enabled && fb_fd >= 0 && clock_gettime(CLOCK_MONOTONIC, &resize_now) == 0)
        {
            static struct timespec last_resize_check{};
            const int64_t elapsed_us = (resize_now.tv_sec - last_resize_check.tv_sec) * 1000000 +
                                       (resize_now.tv_nsec - last_resize_check.tv_nsec) / 1000;
            if ((last_resize_check.tv_sec == 0 && last_resize_check.tv_nsec == 0) || elapsed_us >= 2000000)
            {
                last_resize_check = resize_now;
                fb_var_screeninfo current_var{};
                fb_fix_screeninfo current_fix{};
                if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &current_var) == 0 &&
                    ioctl(fb_fd, FBIOGET_FSCREENINFO, &current_fix) == 0)
                {
                    const int current_rows = static_cast<int>(current_var.yres / 16);
                    const int current_cols = static_cast<int>(current_var.xres / 8);
                    const int current_pitch = static_cast<int>(current_fix.line_length / sizeof(std::uint32_t));
                    const bool changed = current_rows != rows || current_cols != cols ||
                                         current_pitch != pitch_pixels || current_var.xres != var.xres ||
                                         current_var.yres != var.yres;
                    if (changed && current_var.bits_per_pixel == 32 && current_var.xres >= 8 &&
                        current_var.yres >= 16 && current_fix.line_length >= current_var.xres * sizeof(std::uint32_t))
                    {
                        const std::uint64_t current_visible_frame_bytes =
                            static_cast<std::uint64_t>(current_fix.line_length) * current_var.yres;
                        if (current_fix.smem_len < current_visible_frame_bytes)
                        {
                            _s_log("consoled: framebuffer memory is smaller than visible frame\n");
                            continue;
                        }
                        const std::uint64_t current_mapped_bytes =
                            (static_cast<std::uint64_t>(current_fix.smem_len) + page_size - 1) & ~(page_size - 1);
                        auto *new_backbuffer = static_cast<std::uint32_t *>(
                            calloc(static_cast<std::size_t>(current_rows * 16) * current_pitch, sizeof(std::uint32_t)));
                        if (new_backbuffer == nullptr)
                        {
                            _s_log("consoled: resize backbuffer allocation failed\n");
                        }
                        else
                        {
                            auto *new_scanout = scanout;
                            if (current_mapped_bytes != mapped_bytes)
                            {
                                new_scanout = static_cast<std::uint32_t *>(
                                    mmap(nullptr, current_mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0));
                            }
                            if (new_scanout == MAP_FAILED)
                            {
                                free(new_backbuffer);
                                _s_log("consoled: resize framebuffer mapping failed\n");
                                continue;
                            }
                            if (new_scanout != scanout)
                                (void)munmap(scanout, mapped_bytes);
                            free(backbuffer);
                            backbuffer = new_backbuffer;
                            scanout = new_scanout;
                            mapped_bytes = current_mapped_bytes;
                            fix = current_fix;
                            var = current_var;
                            rows = current_rows;
                            cols = current_cols;
                            pitch_pixels = current_pitch;
                            render_state.rows = rows;
                            render_state.cols = cols;
                            render_state.pitch_pixels = pitch_pixels;
                            render_state.backbuffer = backbuffer;
                            vterm_set_size(vt, current_rows, current_cols);
                            vterm_screen_flush_damage(screen);
                            struct winsize resize_ws{};
                            resize_ws.ws_row = static_cast<unsigned short>(current_rows);
                            resize_ws.ws_col = static_cast<unsigned short>(current_cols);
                            resize_ws.ws_xpixel = static_cast<unsigned short>(current_var.xres);
                            resize_ws.ws_ypixel = static_cast<unsigned short>(current_var.yres);
                            if (master_fd >= 0)
                                (void)ioctl(master_fd, TIOCSWINSZ, &resize_ws);
                            else
                                (void)master_set_winsize(master, current_rows, current_cols, current_var.xres,
                                                         current_var.yres);
                            add_damage(render_state, {0, rows, 0, cols});
                            if (framebuffer_enabled)
                                render_damage(screen, render_state, scanout);
                            _s_log("consoled: framebuffer resize applied\n");
                        }
                    }
                }
            }
        }

        if (master_fd < 0)
        {
            if (master_read_request.kind == master_async_kind::none &&
                submit_master_watch(master, master_read_request, terminal_readable | terminal_hangup) != 0)
            {
                _s_log("consoled: cannot submit master read watch\n");
                goto shutdown;
            }
            if (master_write_request.kind == master_async_kind::none && pending_master_output_size != 0)
            {
                const std::size_t write_size = pending_master_output_size < sizeof(master_write_request.wire) / 2
                                                   ? pending_master_output_size
                                                   : sizeof(master_write_request.wire) / 2;
                const int submit_error =
                    submit_master_write(master, master_write_request, pending_master_output, write_size);
                if (submit_error != 0 && submit_error != EAGAIN)
                {
                    _s_log("consoled: cannot submit master write\n");
                    goto shutdown;
                }
            }
        }

        constexpr std::uint64_t invalid_wait_index = static_cast<std::uint64_t>(-1);
        na_wait_item_t wait_items[4]{};
        uint64_t wait_count = 0;
        const uint64_t master_read_wait_index =
            master_fd < 0 && master_read_request.kind != master_async_kind::none ? wait_count : invalid_wait_index;
        if (master_read_wait_index != invalid_wait_index)
            wait_items[wait_count++] = {master_read_request.invocation, NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED, 0};
        const uint64_t master_write_wait_index =
            master_fd < 0 && master_write_request.kind != master_async_kind::none ? wait_count : invalid_wait_index;
        if (master_write_wait_index != invalid_wait_index)
            wait_items[wait_count++] = {master_write_request.invocation, NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED,
                                        0};
        const uint64_t master_write_retry_index =
            master_fd < 0 && master_write_request.kind == master_async_kind::none && pending_master_output_size != 0
                ? wait_count
                : invalid_wait_index;
        if (master_write_retry_index != invalid_wait_index)
            wait_items[wait_count++] = {master, NA_SIGNAL_WRITABLE | NA_SIGNAL_PEER_CLOSED, 0};
        const uint64_t input_wait_index = input_receiver != NA_HANDLE_INVALID ? wait_count : invalid_wait_index;
        if (input_receiver != NA_HANDLE_INVALID)
            wait_items[wait_count++] = {input_receiver, NA_SIGNAL_READABLE | NA_SIGNAL_PEER_CLOSED, 0};
        if (wait_count == 0)
        {
            _s_log("consoled: no wait handles\n");
            break;
        }

        struct timespec deadline{};
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        {
            _s_log("consoled: clock_gettime failed\n");
            break;
        }
        const auto retry_deadline = input_receiver == NA_HANDLE_INVALID ? input_retry_after : 0;
        if (retry_deadline != 0)
        {
            const auto now_ms = static_cast<std::uint64_t>(deadline.tv_sec) * 1000 + deadline.tv_nsec / 1'000'000;
            if (retry_deadline < now_ms)
            {
                deadline.tv_sec = static_cast<time_t>(retry_deadline / 1000);
                deadline.tv_nsec = static_cast<long>((retry_deadline % 1000) * 1'000'000);
            }
            else
                deadline.tv_sec += 2;
        }
        else
            deadline.tv_sec += 2;
        const auto wait_status = loop.wait(wait_items, wait_count, &deadline);
        if (wait_status == NA_STATUS_WAIT_TIMED_OUT)
            continue;
        if (wait_status != NA_STATUS_OK)
        {
            char message[96]{};
            snprintf(message, sizeof(message), "consoled: wait failed status=%d\n", static_cast<int>(wait_status));
            _s_log(message);
            break;
        }
        if (master_write_retry_index != invalid_wait_index &&
            (wait_items[master_write_retry_index].observed & NA_SIGNAL_PEER_CLOSED) != 0)
        {
            _s_log("consoled: master endpoint closed\n");
            goto shutdown;
        }

        std::uint8_t buffer[1024]{};
        if (master_read_wait_index != invalid_wait_index &&
            (wait_items[master_read_wait_index].observed & (NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED)) != 0)
        {
            const auto kind = master_read_request.kind;
            std::size_t read_size = 0;
            bool hangup = false;
            bool readable = false;
            int read_error =
                complete_master_read(master, master_read_request, buffer, sizeof(buffer), read_size, hangup, readable);
            if (read_error == 0 && kind == master_async_kind::read_watch)
            {
                if (!readable && hangup)
                {
                    _s_log("consoled: master hangup\n");
                    goto shutdown;
                }
                read_error = submit_master_read(master, master_read_request);
            }
            if (read_error != 0)
            {
                char message[96]{};
                snprintf(message, sizeof(message), "consoled: master read failed errno=%d\n", read_error);
                _s_log(message);
                goto shutdown;
            }
            if (kind == master_async_kind::read && read_size != 0)
            {
                vterm_input_write(vt, reinterpret_cast<const char *>(buffer), read_size);
                vterm_screen_flush_damage(screen);
                if (framebuffer_enabled)
                    render_damage(screen, render_state, scanout);
            }
            if (kind == master_async_kind::read && read_size == 0 && !hangup)
            {
                // A non-zero terminal read request has no EOF state on the
                // master side. Treat an empty completed invocation as a
                // transient protocol race and re-arm the readiness watch;
                // tearing down the frontend here hangs up the foreground
                // shell and turns the display black.
                _s_log("consoled: empty master read; retrying\n");
                continue;
            }
        }

        if (master_write_wait_index != invalid_wait_index &&
            (wait_items[master_write_wait_index].observed & (NA_SIGNAL_COMPLETED | NA_SIGNAL_PEER_CLOSED)) != 0)
        {
            const auto kind = master_write_request.kind;
            std::size_t written = 0;
            bool would_block = false;
            int write_error = complete_master_write(master, master_write_request, written, would_block);
            if (write_error != 0)
            {
                char message[96]{};
                snprintf(message, sizeof(message), "consoled: master write failed errno=%d\n", write_error);
                _s_log(message);
                goto shutdown;
            }
            if (kind == master_async_kind::write)
            {
                if (written > pending_master_output_size)
                {
                    _s_log("consoled: invalid master write count\n");
                    goto shutdown;
                }
                if (written != 0)
                {
                    pending_master_output_size -= written;
                    std::memmove(pending_master_output, pending_master_output + written, pending_master_output_size);
                }
                if (would_block && pending_master_output_size != 0 &&
                    submit_master_watch(master, master_write_request, terminal_writable) != 0)
                {
                    _s_log("consoled: cannot submit master write watch\n");
                    goto shutdown;
                }
            }
        }

        if (master_fd >= 0)
        {
            struct pollfd poll_item{master_fd, POLLIN | POLLHUP | POLLERR, 0};
            if (poll(&poll_item, 1, 0) > 0 && (poll_item.revents & (POLLHUP | POLLERR)) != 0)
                break;
            if (poll_item.revents & POLLIN)
            {
                for (;;)
                {
                    const int n = static_cast<int>(read(master_fd, buffer, sizeof(buffer)));
                    if (n > 0)
                    {
                        vterm_input_write(vt, reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(n));
                        vterm_screen_flush_damage(screen);
                        if (framebuffer_enabled)
                            render_damage(screen, render_state, scanout);
                        continue;
                    }
                    if (n < 0 && errno == EAGAIN)
                        break;
                    if (n == 0 || (n < 0 && errno != EAGAIN))
                        goto shutdown;
                }
            }
        }

        if (input_wait_index == invalid_wait_index ||
            (wait_items[input_wait_index].observed & (NA_SIGNAL_READABLE | NA_SIGNAL_PEER_CLOSED)) == 0)
            continue;
        if ((wait_items[input_wait_index].observed & NA_SIGNAL_PEER_CLOSED) != 0)
        {
            (void)naos_handle_close(input_receiver);
            input_receiver = NA_HANDLE_INVALID;
            console_capslock = false;
            console_numlock = false;
            console_scrolllock = false;
            console_compose_pending = false;
            console_modifier_state = 0;
            std::memset(console_pressed, 0, sizeof(console_pressed));
            _s_log("consoled: input receiver closed\n");
            continue;
        }

        for (;;)
        {
            na_channel_receive_frame_t frame{};
            frame.struct_size = sizeof(frame);
            frame.byte_capacity = sizeof(buffer);
            frame.resource_capacity = 0;
            frame.bytes = reinterpret_cast<std::uint64_t>(buffer);
            const auto receive_status = _na_channel_receive(input_receiver, &frame);
            if (receive_status != NA_STATUS_OK)
                break;

            naos::canonical::reader reader(buffer, frame.actual_bytes);
            naos::system::InputEventSource::KeyEvent event{};
            naos::system::InputEventSource::KeyEvent_decode(reader, event);
            if (!reader.good())
                continue;
            if (event.kind == NA_INPUT_EVENT_KIND_FRAMEBUFFER_DISABLE)
            {
                disable_framebuffer();
                continue;
            }
            if (event.kind == NA_INPUT_EVENT_KIND_FRAMEBUFFER_ENABLE)
            {
                (void)enable_framebuffer();
                continue;
            }
            if (event.kind == NA_INPUT_EVENT_KIND_OVERRUN)
            {
                console_capslock = false;
                console_numlock = false;
                console_scrolllock = false;
                console_compose_pending = false;
                console_modifier_state = 0;
                std::memset(console_pressed, 0, sizeof(console_pressed));
                _s_log("consoled: input overrun; keyboard state reset\n");
                continue;
            }
            const unsigned key_index = static_cast<unsigned>(event.key_code) & 0xff;
            const auto key = static_cast<console_key>(event.key_code);
            if (event.kind == NA_INPUT_EVENT_KIND_RELEASE)
            {
                console_pressed[key_index] = false;
                std::uint8_t modifier = 0;
                if (is_modifier_key(key, modifier))
                    console_modifier_state = static_cast<std::uint8_t>(console_modifier_state & ~modifier);
                continue;
            }
            const bool repeat = event.kind == NA_INPUT_EVENT_KIND_REPEAT;
            if (event.kind != NA_INPUT_EVENT_KIND_PRESS && !repeat)
                continue;
            if (!repeat)
            {
                console_pressed[key_index] = true;
                std::uint8_t modifier = 0;
                if (is_modifier_key(key, modifier))
                    console_modifier_state = static_cast<std::uint8_t>(console_modifier_state | modifier);
            }
            else if (!console_pressed[key_index])
                continue;
            emit_key_event(vt, event);
            std::uint8_t mapped[128]{};
            const std::size_t mapped_size = vterm_output_read(vt, reinterpret_cast<char *>(mapped), sizeof(mapped));
            if (mapped_size != 0)
            {
                if (master_fd >= 0)
                    (void)write(master_fd, mapped, mapped_size);
                else
                {
                    if (mapped_size > sizeof(pending_master_output) - pending_master_output_size)
                    {
                        _s_log("consoled: master output queue full\n");
                        goto shutdown;
                    }
                    std::memcpy(pending_master_output + pending_master_output_size, mapped, mapped_size);
                    pending_master_output_size += mapped_size;
                }
            }
        }
    }

shutdown:

    close_master_async_request(master_read_request);
    close_master_async_request(master_write_request);
    free(backbuffer);
    if (scanout != nullptr && scanout != MAP_FAILED)
        (void)munmap(scanout, mapped_bytes);
    if (fb_fd >= 0)
        close(fb_fd);
    if (master_fd >= 0)
        close(master_fd);
    else
        (void)naos_handle_close(master);
    if (input_receiver != NA_HANDLE_INVALID)
        (void)naos_handle_close(input_receiver);
    if (console_frontend != NA_HANDLE_INVALID)
        (void)naos_handle_close(console_frontend);
    if (input_event_source != NA_HANDLE_INVALID)
        (void)naos_handle_close(input_event_source);
    std::printf("consoled: rendered\n");
    _s_log("consoled: rendered\n");
    return shutdown_status;
}
