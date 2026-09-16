#include "kernel/log.hpp"
#include "freelibcxx/string.hpp"
#include "kernel/arch/com.hpp"
#include "kernel/arch/cpu.hpp"
#include "kernel/arch/io.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/cmdline.hpp"
#include "kernel/cpu.hpp"
#include "kernel/log_format.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/scheduler.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"

namespace log
{
KLOG_MODULE(kernel);

namespace
{
constexpr u64 early_storage_size = 8 * 1024;
constexpr u64 runtime_storage_size = 1024 * 1024;
constexpr u64 max_rendered_record = 768;

enum class logger_state : u8
{
    early,
    runtime,
    panic,
};

enum class sink_id : u8
{
    console,
    serial,
    dmesg,
    count,
};

struct filter_entry
{
    bool present = false;
    level minimum = level::info;
};

struct worker_state
{
    u64 cursor = 1;
    u64 retry_after = 0;
    u64 gap_offset = 0;
    u64 gap_length = 0;
    task::wait_queue_t *wait = nullptr;
    bool started = false;
    bool gap_pending = false;
    char rendered[max_rendered_record]{};
    char plain[max_rendered_record]{};
    char colored[max_rendered_record + 128]{};
};

constexpr u64 record_size = sizeof(record);
constexpr u64 early_slot_count = early_storage_size / record_size > 0 ? early_storage_size / record_size : 1;
constexpr u64 runtime_slot_count = runtime_storage_size / record_size > 0 ? runtime_storage_size / record_size : 1;
constexpr u64 max_cpu_count = arch::cpu::max_cpu_support;

record early_ring[early_slot_count]{};
record runtime_ring[runtime_slot_count]{};
configuration current_configuration{};
filter_entry filters[static_cast<u8>(module::test) + 1]{};
worker_state workers[static_cast<u8>(sink_id::count)]{};
char emergency_buffers[arch::cpu::max_cpu_support][max_rendered_record]{};
char early_console_buffers[max_cpu_count][max_rendered_record + 128]{};
char nmi_buffers[max_cpu_count][max_rendered_record]{};
std::atomic<u16> nmi_buffer_lengths[max_cpu_count]{};
std::atomic<u64> nmi_dropped{0};

lock::spinlock_t core_lock;
lock::spinlock_t emergency_lock;
std::atomic<u64> committed_sequence{0};
std::atomic<u64> emergency_sequence{0};
std::atomic_bool panic_active{false};
std::atomic_bool workers_started{false};
std::atomic<logger_state> state{logger_state::early};
u64 next_sequence = 1;
u64 early_head = 0;
u64 early_count = 0;
u64 runtime_head = 0;
u64 runtime_count = 0;
u64 runtime_slots = 1;
u64 early_slots = early_slot_count;
std::atomic<u64> total_dropped{0};
u64 invalid_configuration = 0;
bool serial_ready = false;
arch::device::com::serial serial_device;

char *emergency_buffer()
{
    u32 cpu_id = 0;
    if (cpu::has_init())
        cpu_id = cpu::current().id();
    if (cpu_id >= arch::cpu::max_cpu_support)
        cpu_id = 0;
    return emergency_buffers[cpu_id];
}

u32 current_cpu_id()
{
    if (!cpu::has_init())
        return 0;
    const u32 cpu_id = cpu::current().id();
    return cpu_id < max_cpu_count ? cpu_id : 0;
}

const char *const level_names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "PANIC"};
const char *const module_names[] = {"kernel", "arch", "acpi", "mm", "task", "sched",
                                    "ipc",    "fs",   "dev",  "io", "tty",  "test"};

bool equal(const char *value, u64 length, const char *literal)
{
    const u64 literal_length = strlen(literal);
    return length == literal_length && memcmp(value, literal, length) == 0;
}

bool read_early(const char *key, char *&value, u64 &length) { return cmdline::early_get(key, value, length); }

bool parse_level_value(const char *value, u64 length, level &result)
{
    if (equal(value, length, "trace"))
        result = level::trace;
    else if (equal(value, length, "debug"))
        result = level::debug;
    else if (equal(value, length, "info"))
        result = level::info;
    else if (equal(value, length, "warning") || equal(value, length, "warn"))
        result = level::warning;
    else if (equal(value, length, "error"))
        result = level::error;
    else if (equal(value, length, "panic"))
        result = level::panic;
    else
        return false;
    return true;
}

bool parse_bool_value(const char *value, u64 length, bool &result)
{
    if (equal(value, length, "on") || equal(value, length, "true"))
        result = true;
    else if (equal(value, length, "off") || equal(value, length, "false"))
        result = false;
    else
        return false;
    return true;
}

bool parse_module_value(const char *value, u64 length, module &result)
{
    for (u8 index = 0; index <= static_cast<u8>(module::test); index++)
    {
        if (equal(value, length, module_names[index]))
        {
            result = static_cast<module>(index);
            return true;
        }
    }
    return false;
}

bool parse_size_value(const char *value, u64 length, u64 minimum, u64 maximum, u64 &result)
{
    if (length == 0)
        return false;
    u64 number = 0;
    u64 index = 0;
    while (index < length && value[index] >= '0' && value[index] <= '9')
    {
        const u64 digit = static_cast<u64>(value[index] - '0');
        if (number > (maximum >> 1) && digit != 0)
            return false;
        number = number * 10 + digit;
        index++;
    }
    u64 multiplier = 1;
    if (index < length)
    {
        if (index + 1 != length)
            return false;
        if (value[index] == 'k' || value[index] == 'K')
            multiplier = 1024;
        else if (value[index] == 'm' || value[index] == 'M')
            multiplier = 1024 * 1024;
        else
            return false;
    }
    if (number == 0 || number > maximum / multiplier)
        return false;
    result = number * multiplier;
    return result >= minimum && result <= maximum;
}

void record_configuration_warning(const char *message)
{
    const char prefix[] = "invalid kernel_log configuration: ";
    char buffer[128]{};
    freelibcxx::format_detail::writer output{freelibcxx::span<char>(buffer, sizeof(buffer) - 1)};
    output.put(prefix);
    output.put(message);
    detail::emit_message(level::warning, module::kernel, __FILE__, __LINE__, false, buffer, output.written);
}

bool command_line_has_flag(const char *flag) { return cmdline::early_has_flag(flag); }

void parse_filters(const char *value, u64 length)
{
    if (length > 256)
    {
        invalid_configuration++;
        return;
    }

    u64 entry_start = 0;
    u64 entry_count = 0;
    while (entry_start < length)
    {
        u64 entry_end = entry_start;
        while (entry_end < length && value[entry_end] != ',')
            entry_end++;
        if (entry_count++ >= 16)
        {
            invalid_configuration++;
            entry_start = entry_end + 1;
            continue;
        }

        u64 separator = entry_start;
        while (separator < entry_end && value[separator] != '=')
            separator++;
        module source{};
        level minimum{};
        u64 level_start = separator + 1;
        if (separator == entry_end || !parse_module_value(value + entry_start, separator - entry_start, source) ||
            !parse_level_value(value + level_start, entry_end - level_start, minimum) ||
            filters[static_cast<u8>(source)].present)
        {
            invalid_configuration++;
        }
        else
        {
            filters[static_cast<u8>(source)] = {true, minimum};
        }
        entry_start = entry_end + 1;
    }
}

void parse_sinker(const char *value, u64 length)
{
    u64 start = 0;
    u64 entries = 0;
    bool seen_console = false;
    bool seen_serial = false;
    bool seen_dmesg = false;
    while (start < length)
    {
        u64 end = start;
        while (end < length && value[end] != ',')
            end++;
        if (entries++ >= 8)
        {
            invalid_configuration++;
            start = end + 1;
            continue;
        }

        u64 fields[3]{};
        u64 field_count = 0;
        for (u64 pos = start; pos < end; pos++)
        {
            if (value[pos] == ':')
            {
                if (field_count >= 3)
                {
                    field_count = 99;
                    break;
                }
                fields[field_count++] = pos;
            }
        }

        const bool shape_ok = field_count == 3;
        const u64 name_end = shape_ok ? fields[0] : start;
        const u64 on_start = shape_ok ? fields[0] + 1 : start;
        const u64 on_end = shape_ok ? fields[1] : start;
        const u64 level_start = shape_ok ? fields[1] + 1 : start;
        const u64 level_end = shape_ok ? fields[2] : start;
        const u64 color_start = shape_ok ? fields[2] + 1 : start;

        level minimum{};
        bool enabled_value = false;
        const bool valid = shape_ok && parse_level_value(value + level_start, level_end - level_start, minimum) &&
                           parse_bool_value(value + on_start, on_end - on_start, enabled_value) &&
                           (equal(value + color_start, end - color_start, "color") ||
                            equal(value + color_start, end - color_start, "nocolor"));
        if (!valid)
        {
            invalid_configuration++;
            start = end + 1;
            continue;
        }

        sink_config *sink = nullptr;
        bool *seen = nullptr;
        if (equal(value + start, name_end - start, "console"))
        {
            sink = &current_configuration.console;
            seen = &seen_console;
        }
        else if (equal(value + start, name_end - start, "serial"))
        {
            sink = &current_configuration.serial;
            seen = &seen_serial;
        }
        else if (equal(value + start, name_end - start, "dmesg"))
        {
            sink = &current_configuration.dmesg;
            seen = &seen_dmesg;
        }

        if (sink == nullptr || *seen)
        {
            invalid_configuration++;
        }
        else
        {
            *seen = true;
            sink->enabled = enabled_value;
            sink->minimum = minimum;
            sink->color = equal(value + color_start, end - color_start, "color");
        }
        start = end + 1;
    }
}

void load_configuration()
{
    current_configuration = configuration{};
    for (auto &filter : filters)
        filter = {};
    invalid_configuration = 0;

    char *value = nullptr;
    u64 length = 0;
    if (read_early("kernel_log_level", value, length) &&
        !parse_level_value(value, length, current_configuration.retention_level))
        invalid_configuration++;
    if (read_early("kernel_log_emergency_serial", value, length) &&
        !parse_bool_value(value, length, current_configuration.emergency_serial))
        invalid_configuration++;
    if (read_early("kernel_log_filter", value, length))
        parse_filters(value, length);
    if (read_early("kernel_log_sinker", value, length))
        parse_sinker(value, length);

    if (command_line_has_flag("quiet"))
        current_configuration.console.enabled = false;
}

bool level_at_least(level value, level minimum) { return static_cast<u8>(value) >= static_cast<u8>(minimum); }

record *active_ring() { return state == logger_state::early ? early_ring : runtime_ring; }

u64 active_slots() { return state == logger_state::early ? early_slots : runtime_slots; }

u64 &active_head() { return state == logger_state::early ? early_head : runtime_head; }

u64 &active_count() { return state == logger_state::early ? early_count : runtime_count; }

void copy_text(char *destination, u64 capacity, const char *source, u64 length, bool &truncated)
{
    const u64 copied = length < capacity ? length : capacity;
    if (copied != 0)
        memcpy(destination, source, copied);
    destination[copied] = 0;
    truncated = copied != length;
}

void build_record(record &result, level severity, module source, const char *file, u32 line, bool raw,
                  const char *message, u64 length)
{
    result = {};
    result.header.level = static_cast<u8>(severity);
    result.header.module = static_cast<u8>(source);
    result.header.line = line;
    result.header.flags = raw ? raw_record : 0;

    bool file_was_truncated = false;
    copy_text(result.file, max_file_length, file, strlen(file), file_was_truncated);
    result.header.file_length = static_cast<u8>(strlen(result.file));
    if (file_was_truncated)
        result.header.flags |= log::file_truncated;

    if (cpu::has_init())
    {
        result.header.cpu_id = cpu::current().id();
        if (task::has_init())
        {
            auto *thread = task::current();
            if (thread != nullptr)
            {
                result.header.pid = thread->process->pid;
                result.header.tid = thread->tid;
            }
            else
                result.header.flags |= unknown_context;
        }
        else
            result.header.flags |= unknown_context;
        if (cpu::current().get_event_clock() != nullptr)
            result.header.timestamp = timer::get_high_resolution_time();
        else
            result.header.flags |= early_record;
    }
    else
    {
        result.header.flags |= early_record | unknown_context;
    }

    freelibcxx::format_detail::writer output{freelibcxx::span<char>(result.message, max_message_length)};
    if (raw)
        output.put(message, length);
    else
        format::append_escaped(output, message, length);
    result.header.message_length = static_cast<u16>(output.written);
    if (output.truncated)
        result.header.flags |= message_truncated;
}

void build_user_record(record &result, const char *process_name, const char *file, u32 line, bool raw,
                       const char *message, u64 length)
{
    char normalized_name[max_process_name_length + 1]{};
    bool process_name_was_truncated = false;
    const char *name = process_name == nullptr ? "process" : process_name;
    copy_text(normalized_name, max_process_name_length, name, strlen(name), process_name_was_truncated);

    const char *payload = message;
    u64 payload_length = length;
    const u64 name_length = strlen(normalized_name);
    if (name_length != 0 && length >= name_length + 2 && memcmp(message, normalized_name, name_length) == 0 &&
        message[name_length] == ':' && message[name_length + 1] == ' ')
    {
        payload += name_length + 2;
        payload_length -= name_length + 2;
    }

    build_record(result, level::info, module::io, file, line, raw, payload, payload_length);
    result.header.flags |= user_record;
    memcpy(result.process_name, normalized_name, sizeof(normalized_name));
}

void build_nmi_record(record &result, module source, const char *file, u32 line, const char *message, u64 length)
{
    result = {};
    result.header.level = static_cast<u8>(level::debug);
    result.header.module = static_cast<u8>(source);
    result.header.line = line;
    result.header.cpu_id = current_cpu_id();
    result.header.flags = nmi_record | unknown_context;

    bool file_was_truncated = false;
    copy_text(result.file, max_file_length, file, strlen(file), file_was_truncated);
    result.header.file_length = static_cast<u8>(strlen(result.file));
    if (file_was_truncated)
        result.header.flags |= file_truncated;

    freelibcxx::format_detail::writer output{freelibcxx::span<char>(result.message, max_message_length)};
    format::append_escaped(output, message, length);
    result.header.message_length = static_cast<u16>(output.written);
    if (output.truncated)
        result.header.flags |= message_truncated;
}

bool raw_message_is_blank(const char *message, u64 length)
{
    for (u64 index = 0; index < length; index++)
    {
        const char value = message[index];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
            return false;
    }
    return true;
}

void append_integer(freelibcxx::format_detail::writer &output, u64 value, size_t width = 0)
{
    freelibcxx::format_detail::append_integer(output, value, 0, width);
}

u64 oldest_sequence_locked()
{
    record *ring = active_ring();
    const u64 slots = active_slots();
    u64 oldest = 0;
    for (u64 index = 0; index < slots; index++)
    {
        if (ring[index].header.sequence != 0 && (oldest == 0 || ring[index].header.sequence < oldest))
            oldest = ring[index].header.sequence;
    }
    return oldest;
}

void commit_record_locked(record &result)
{
    const u64 sequence = next_sequence++;
    record *ring = active_ring();
    const u64 slots = active_slots();
    u64 &head = active_head();
    u64 &count = active_count();
    if (count == slots)
        total_dropped.fetch_add(1, std::memory_order_relaxed);
    // Publish the sequence only after the complete slot has been copied. The
    // panic snapshot may read slots without taking core_lock.
    __atomic_store_n(&ring[head].header.sequence, u64{0}, __ATOMIC_RELAXED);
    result.header.sequence = 0;
    ring[head] = result;
    __atomic_store_n(&ring[head].header.sequence, sequence, __ATOMIC_RELEASE);
    result.header.sequence = sequence;
    head = (head + 1) % slots;
    if (count < slots)
        count++;
    committed_sequence.store(sequence, std::memory_order_release);
}

void commit_record(record &result)
{
    core_lock.lock();
    commit_record_locked(result);
    core_lock.unlock();
}

void write_debugcon(const char *data, u64 length)
{
    for (u64 index = 0; index < length; index++)
        io_out8(0xe9, static_cast<u8>(data[index]));
}

void emergency_write(const char *data, u64 length)
{
    if (!emergency_lock.try_lock())
    {
        write_debugcon(data, length);
        return;
    }

    bool serial_failed = false;
    if (current_configuration.emergency_serial && serial_ready)
    {
        for (u64 index = 0; index < length; index++)
        {
            if (!serial_device.try_write(static_cast<byte>(data[index]), 1000))
            {
                serial_failed = true;
                break;
            }
        }
    }
    if (!current_configuration.emergency_serial || !serial_ready || serial_failed)
        write_debugcon(data, length);
    emergency_lock.unlock();
}

void emergency_write_panic(const char *data, u64 length)
{
    if (!current_configuration.serial.color)
    {
        emergency_write(data, length);
        return;
    }
    static constexpr char red[] = "\x1b[91m";
    static constexpr char reset[] = "\x1b[0m";
    emergency_write(red, sizeof(red) - 1);
    emergency_write(data, length);
    emergency_write(reset, sizeof(reset) - 1);
}

void console_write_panic(const char *data, u64 length)
{
    if (term::early_terminal == nullptr)
        return;

    static constexpr char red[] = "\x1b[91m";
    static constexpr char reset[] = "\x1b[0m";
    term::reset_panic_term();
    term::write_to_klog(freelibcxx::const_string_view(red, sizeof(red) - 1));
    term::write_to_klog(freelibcxx::const_string_view(data, length));
    term::write_to_klog(freelibcxx::const_string_view(reset, sizeof(reset) - 1));
}

void write_panic(const char *data, u64 length)
{
    emergency_write_panic(data, length);
    console_write_panic(data, length);
}

u8 color_code(level severity)
{
    switch (severity)
    {
        case level::trace:
            return 90;
        case level::debug:
            return 33;
        case level::info:
            return 32;
        case level::warning:
            return 96;
        case level::error:
        case level::panic:
            return 91;
    }
    return 39;
}

using output_writer = freelibcxx::format_detail::writer;

constexpr u8 metadata_color = 90;
constexpr u8 process_id_color = 33;
constexpr u8 module_color = 36;

void begin_color(output_writer &output, u8 color)
{
    output.put("\e[");
    append_integer(output, color);
    output.put('m');
}

void end_color(output_writer &output) { output.put("\e[0m"); }

void put_colored(output_writer &output, u8 color, const char *value, u64 length)
{
    begin_color(output, color);
    output.put(value, length);
    end_color(output);
}

void put_colored_integer(output_writer &output, u8 color, u64 value, u64 width = 0)
{
    begin_color(output, color);
    append_integer(output, value, width);
    end_color(output);
}

u64 render_record(const record &item, char *buffer, u64 capacity, bool color = false)
{
    freelibcxx::format_detail::writer output{freelibcxx::span<char>(buffer, capacity)};
    const auto severity = static_cast<level>(item.header.level);
    if (!record_has_metadata_prefix(severity))
    {
        output.put("PANIC: ");
        output.put(item.message, item.header.message_length);
        if (item.header.message_length == 0 || item.message[item.header.message_length - 1] != '\n')
            output.put('\n');
        return output.written;
    }
    if (item.header.flags & nmi_record)
    {
        if (color)
        {
            put_colored(output, metadata_color, "[nmi]", 5);
            output.put(' ');
            put_colored_integer(output, metadata_color, item.header.cpu_id);
            put_colored(output, metadata_color, "-?-?", 4);
        }
        else
        {
            output.put("[nmi] ");
            append_integer(output, item.header.cpu_id);
            output.put("-?-?");
        }
    }
    else if (item.header.flags & early_record)
    {
        if (color)
        {
            put_colored(output, metadata_color, "[early]", 7);
            output.put(' ');
            put_colored(output, metadata_color, "?-?-?", 5);
        }
        else
            output.put("[early] ?-?-?");
    }
    else
    {
        if (color)
        {
            begin_color(output, metadata_color);
            output.put('[');
            append_integer(output, item.header.timestamp, 12);
            output.put(']');
            end_color(output);
            output.put(' ');
            begin_color(output, metadata_color);
            append_integer(output, item.header.cpu_id);
            end_color(output);
            output.put('-');
            begin_color(output, process_id_color);
            if (item.header.flags & unknown_context)
                output.put('?');
            else
                append_integer(output, item.header.pid);
            end_color(output);
            output.put('-');
            begin_color(output, metadata_color);
            if (item.header.flags & unknown_context)
                output.put('?');
            else
                append_integer(output, item.header.tid);
            end_color(output);
        }
        else
        {
            output.put('[');
            append_integer(output, item.header.timestamp, 12);
            output.put("] ");
            append_integer(output, item.header.cpu_id);
            output.put('-');
            if (item.header.flags & unknown_context)
                output.put('?');
            else
                append_integer(output, item.header.pid);
            output.put('-');
            if (item.header.flags & unknown_context)
                output.put('?');
            else
                append_integer(output, item.header.tid);
        }
    }
    output.put(' ');
    const char *severity_name = level_name(severity);
    if (color)
        put_colored(output, color_code(severity), severity_name, strlen(severity_name));
    else
        output.put(severity_name);
    output.put(' ');
    const char *source_name = (item.header.flags & user_record) != 0
                                  ? item.process_name
                                  : module_name(static_cast<module>(item.header.module));
    if (color)
        put_colored(output, module_color, source_name, strlen(source_name));
    else
        output.put(source_name);
    output.put(": ");
    output.put(item.message, item.header.message_length);
    if (item.header.message_length == 0 || item.message[item.header.message_length - 1] != '\n')
        output.put('\n');
    return output.written;
}

u64 strip_ansi(const char *source, u64 length, char *destination, u64 capacity)
{
    u64 written = 0;
    bool escape = false;
    for (u64 index = 0; index < length; index++)
    {
        const char value = source[index];
        if (escape)
        {
            if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'))
                escape = false;
            continue;
        }
        if (value == '\x1b')
        {
            escape = true;
            continue;
        }
        if (written < capacity)
            destination[written++] = value;
    }
    return written;
}

bool write_sink(sink_id sink, const record &item)
{
    auto &buffers = workers[static_cast<u8>(sink)];
    const bool color_enabled = sink == sink_id::console  ? current_configuration.console.color
                               : sink == sink_id::serial ? current_configuration.serial.color
                               : sink == sink_id::dmesg  ? current_configuration.dmesg.color
                                                         : false;
    char *rendered_buffer = color_enabled ? buffers.colored : buffers.rendered;
    const u64 rendered_length = render_record(
        item, rendered_buffer, color_enabled ? sizeof(buffers.colored) : sizeof(buffers.rendered), color_enabled);
    const char *data = rendered_buffer;
    u64 length = rendered_length;
    const bool preserve_ansi = color_enabled;
    if (!preserve_ansi)
    {
        length = strip_ansi(rendered_buffer, rendered_length, buffers.plain, sizeof(buffers.plain));
        data = buffers.plain;
    }

    if (sink == sink_id::serial)
    {
        if (serial_ready)
            serial_device.write(reinterpret_cast<const byte *>(data), length);
        else
            return false;
    }
    else if (sink == sink_id::console)
    {
        term::write_to_klog(freelibcxx::const_string_view(data, length));
    }
    else if (sink == sink_id::dmesg)
        return false;
    return true;
}

enum class next_result : u8
{
    none,
    record,
    gap,
};

bool direct_emergency_record(sink_id sink, const record &item)
{
    if ((item.header.flags & early_record) == 0)
        return false;
    if (sink == sink_id::console)
        return current_configuration.console.enabled;
    if (sink == sink_id::serial)
        return current_configuration.emergency_serial && serial_ready;
    return false;
}

next_result next_for_worker(sink_id sink, u64 &cursor, record &item, u64 &gap_first, u64 &gap_last)
{
    for (;;)
    {
        if (!core_lock.try_lock())
        {
            // Logger workers run in the real-time class.  Spinning here can
            // starve the same-CPU thread that currently owns the short-lived
            // commit lock, turning a harmless log burst into a system-wide
            // boot stall.
            task::thread_yield();
            return next_result::none;
        }
        const u64 committed = committed_sequence.load(std::memory_order_acquire);
        const u64 oldest = oldest_sequence_locked();
        if (oldest == 0 || cursor > committed)
        {
            core_lock.unlock();
            return next_result::none;
        }
        if (cursor < oldest)
        {
            gap_first = cursor;
            gap_last = oldest - 1;
            cursor = oldest;
            core_lock.unlock();
            return next_result::gap;
        }

        record *ring = active_ring();
        const u64 slots = active_slots();
        bool found = false;
        for (u64 index = 0; index < slots; index++)
        {
            if (ring[index].header.sequence == cursor)
            {
                item = ring[index];
                cursor++;
                found = true;
                break;
            }
        }
        core_lock.unlock();
        if (!found)
        {
            gap_first = cursor;
            gap_last = oldest > cursor ? oldest - 1 : cursor;
            cursor = oldest;
            return next_result::gap;
        }

        const level minimum = sink == sink_id::console  ? current_configuration.console.minimum
                              : sink == sink_id::serial ? current_configuration.serial.minimum
                                                        : current_configuration.dmesg.minimum;
        if (direct_emergency_record(sink, item) || !level_at_least(static_cast<level>(item.header.level), minimum))
            continue;
        return next_result::record;
    }
}

void prepare_gap(worker_state &worker, u64 first, u64 last)
{
    freelibcxx::format_detail::writer output{freelibcxx::span<char>(worker.rendered, sizeof(worker.rendered))};
    output.put("[log] dropped=");
    append_integer(output, last - first + 1);
    output.put(" records before the oldest retained record");
    output.put("\n");
    worker.gap_offset = 0;
    worker.gap_length = output.written;
    worker.gap_pending = true;
}

bool write_gap(sink_id sink, worker_state &worker)
{
    if (sink == sink_id::console)
    {
        term::write_to_klog(freelibcxx::const_string_view(worker.rendered, worker.gap_length));
        worker.gap_offset = worker.gap_length;
        return true;
    }
    if (sink == sink_id::serial)
    {
        if (!serial_ready)
            return false;
        serial_device.write(reinterpret_cast<const byte *>(worker.rendered), worker.gap_length);
        worker.gap_offset = worker.gap_length;
        return true;
    }
    return false;
}

bool has_pending(sink_id, const worker_state &worker)
{
    const u64 committed = committed_sequence.load(std::memory_order_acquire);
    return worker.retry_after != 0 ? committed >= worker.retry_after : worker.cursor <= committed;
}

void worker_entry(task::thread_start_info_t *info)
{
    const void *argument = info == nullptr ? nullptr : info->args;
    const sink_id sink = static_cast<sink_id>(reinterpret_cast<uintptr_t>(argument));
    auto &worker = workers[static_cast<u8>(sink)];
    worker.started = true;
    for (;;)
    {
        bool progressed = false;
        bool failed = false;
        for (;;)
        {
            if (worker.gap_pending)
            {
                progressed = true;
                if (!write_gap(sink, worker))
                {
                    worker.retry_after = committed_sequence.load(std::memory_order_acquire) + 1;
                    failed = true;
                    break;
                }
                worker.gap_pending = false;
                worker.retry_after = 0;
                continue;
            }

            record item{};
            u64 first = 0;
            u64 last = 0;
            const next_result result = next_for_worker(sink, worker.cursor, item, first, last);
            if (result == next_result::none)
                break;
            progressed = true;
            if (result == next_result::gap)
            {
                prepare_gap(worker, first, last);
                if (!write_gap(sink, worker))
                {
                    worker.retry_after = committed_sequence.load(std::memory_order_acquire) + 1;
                    failed = true;
                    break;
                }
                worker.gap_pending = false;
            }
            else
            {
                if (!write_sink(sink, item))
                {
                    worker.cursor--;
                    worker.retry_after = committed_sequence.load(std::memory_order_acquire) + 1;
                    failed = true;
                    break;
                }
                worker.retry_after = 0;
            }
        }
        if (failed)
            progressed = false;
        if (!progressed && worker.wait != nullptr)
            worker.wait->do_wait([&worker, sink] { return has_pending(sink, worker); });
    }
}

void wake_workers()
{
    for (auto &worker : workers)
    {
        if (worker.wait != nullptr)
            worker.wait->do_wake_up();
    }
}
} // namespace

const char *level_name(level value)
{
    const u8 index = static_cast<u8>(value);
    return index <= static_cast<u8>(level::panic) ? level_names[index] : "UNKNOWN";
}

const char *module_name(module value)
{
    const u8 index = static_cast<u8>(value);
    return index <= static_cast<u8>(module::test) ? module_names[index] : "unknown";
}

bool enabled(level value, module source)
{
    if (value == level::panic)
        return true;
    const auto &filter = filters[static_cast<u8>(source)];
    const level minimum = filter.present ? filter.minimum : current_configuration.retention_level;
    return level_at_least(value, minimum);
}

const configuration &config() { return current_configuration; }

void early_init()
{
    load_configuration();
    char *value = nullptr;
    u64 length = 0;
    u64 configured_early_size = early_storage_size;
    if (read_early("kernel_log_early_buffer_size", value, length) &&
        !parse_size_value(value, length, 1024, early_storage_size, configured_early_size))
        configured_early_size = early_storage_size;
    early_slots = configured_early_size / record_size;
    if (early_slots == 0)
        early_slots = 1;

    const bool legacy_serial = cmdline::early_get_bool("kernel_log_serial", false);
    if (current_configuration.emergency_serial || current_configuration.serial.enabled || legacy_serial)
    {
        serial_device.init(arch::device::com::get_control_port(0));
        serial_ready = true;
    }
}

void init()
{
    u64 configured_size = 32 * 1024;
    char *value = nullptr;
    u64 length = 0;
    if (read_early("kernel_log_buffer_size", value, length) &&
        !parse_size_value(value, length, 8 * 1024, runtime_storage_size, configured_size))
        configured_size = 32 * 1024;
    runtime_slots = configured_size / record_size;
    if (runtime_slots == 0)
        runtime_slots = 1;
    if (runtime_slots > runtime_slot_count)
        runtime_slots = runtime_slot_count;

    core_lock.lock();
    for (auto &slot : runtime_ring)
        slot = {};
    runtime_head = 0;
    runtime_count = 0;
    const u64 first = early_count == early_slots ? early_head : 0;
    for (u64 index = 0; index < early_count; index++)
    {
        const u64 early_index = (first + index) % early_slots;
        runtime_ring[runtime_head] = early_ring[early_index];
        runtime_head = (runtime_head + 1) % runtime_slots;
        if (runtime_count < runtime_slots)
            runtime_count++;
    }
    state = logger_state::runtime;
    core_lock.unlock();

    if (invalid_configuration != 0)
        record_configuration_warning("using defaults for malformed entries");
}

void start_workers()
{
    bool expected = false;
    if (!workers_started.compare_exchange_strong(expected, true))
        return;

    const sink_config sinks[] = {current_configuration.console, current_configuration.serial,
                                 current_configuration.dmesg};
    for (u8 index = 0; index < static_cast<u8>(sink_id::count); index++)
    {
        if (!sinks[index].enabled || index == static_cast<u8>(sink_id::dmesg))
            continue;
        workers[index].wait = memory::New<task::wait_queue_t>(memory::KernelCommonAllocatorV);
        if (workers[index].wait == nullptr)
            continue;
        core_lock.lock();
        workers[index].cursor = oldest_sequence_locked();
        if (workers[index].cursor == 0)
            workers[index].cursor = committed_sequence.load(std::memory_order_acquire) + 1;
        core_lock.unlock();
        task::create_thread(task::current_process(), worker_entry, nullptr,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(index)),
                            task::create_thread_flags::real_time_rr);
    }
}

void flush_sinks()
{
    for (u8 index = 0; index < static_cast<u8>(sink_id::count); index++)
    {
        if (workers[index].wait == nullptr)
            continue;
        workers[index].wait->do_wake_up();
    }
}

void detail::emit_message(level severity, module source, const char *file, u32 line, bool raw, const char *message,
                          u64 length)
{
    if (raw && raw_message_is_blank(message, length))
        return;

    record item{};
    if (panic_active.load(std::memory_order_acquire))
    {
        if (raw)
        {
            write_panic(message, length);
            return;
        }
        build_record(item, severity, source, file, line, raw, message, length);
        item.header.sequence = emergency_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        char *buffer = emergency_buffer();
        const u64 rendered = render_record(item, buffer, max_rendered_record);
        write_panic(buffer, rendered);
        return;
    }

    build_record(item, severity, source, file, line, raw, message, length);
    commit_record(item);
    if (item.header.flags & early_record)
    {
        char *buffer = emergency_buffer();
        const u64 rendered = render_record(item, buffer, max_rendered_record);
        emergency_write(buffer, rendered);
        if (current_configuration.console.enabled)
        {
            char *console_buffer = early_console_buffers[current_cpu_id()];
            const u64 console_rendered =
                render_record(item, console_buffer, max_rendered_record + 128, current_configuration.console.color);
            term::write_to_klog(freelibcxx::const_string_view(console_buffer, console_rendered));
        }
    }
    wake_workers();
}

void detail::emit_user_message(const char *process_name, const char *file, u32 line, bool raw, const char *message,
                               u64 length)
{
    if (raw && raw_message_is_blank(message, length))
        return;

    record item{};
    if (panic_active.load(std::memory_order_acquire))
    {
        if (raw)
        {
            write_panic(message, length);
            return;
        }
        build_user_record(item, process_name, file, line, raw, message, length);
        item.header.sequence = emergency_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        char *buffer = emergency_buffer();
        const u64 rendered = render_record(item, buffer, max_rendered_record);
        write_panic(buffer, rendered);
        return;
    }

    build_user_record(item, process_name, file, line, raw, message, length);
    commit_record(item);
    if (item.header.flags & early_record)
    {
        char *buffer = emergency_buffer();
        const u64 rendered = render_record(item, buffer, max_rendered_record);
        emergency_write(buffer, rendered);
        if (current_configuration.console.enabled)
        {
            char *console_buffer = early_console_buffers[current_cpu_id()];
            const u64 console_rendered =
                render_record(item, console_buffer, max_rendered_record + 128, current_configuration.console.color);
            term::write_to_klog(freelibcxx::const_string_view(console_buffer, console_rendered));
        }
    }
    wake_workers();
}

void detail::emit_nmi_message(module source, const char *file, u32 line, const char *message, u64 length)
{
    record item{};
    build_nmi_record(item, source, file, line, message, length);
    if (!panic_active.load(std::memory_order_acquire) && core_lock.try_lock())
    {
        commit_record_locked(item);
        core_lock.unlock();
        wake_workers();
        return;
    }

    const u32 cpu_id = current_cpu_id();
    char *buffer = nmi_buffers[cpu_id];
    freelibcxx::format_detail::writer output{freelibcxx::span<char>(buffer, max_rendered_record - 1)};
    output.put("[nmi] logger busy: ");
    format::append_escaped(output, message, length);
    output.put('\n');
    nmi_buffer_lengths[cpu_id].store(static_cast<u16>(output.written), std::memory_order_release);
    nmi_dropped.fetch_add(1, std::memory_order_relaxed);
    emergency_write(buffer, output.written);
}

void detail::emit_panic_message(module source, const char *file, u32 line, bool raw, const char *message, u64 length,
                                const regs_t *regs)
{
    bool expected = false;
    if (!panic_active.compare_exchange_strong(expected, true))
    {
        static constexpr char reentered[] = "[panic] re-entered panic path\n";
        write_panic(reentered, sizeof(reentered) - 1);
        return;
    }

    state.store(logger_state::panic, std::memory_order_release);
    emergency_sequence.store(committed_sequence.load(std::memory_order_acquire), std::memory_order_relaxed);

    record item{};
    build_record(item, level::panic, source, file, line, raw, message, length);
    item.header.sequence = emergency_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    char *buffer = emergency_buffer();

    static record snapshot[16]{};
    u64 snapshot_count = 0;
    const u64 committed = committed_sequence.load(std::memory_order_acquire);
    {
        record *ring = runtime_ring;
        const u64 slots = runtime_slots;
        for (u64 index = 0; index < slots; index++)
        {
            const u64 before = __atomic_load_n(&ring[index].header.sequence, __ATOMIC_ACQUIRE);
            if (before == 0 || before > committed)
                continue;

            u64 target = snapshot_count;
            if (snapshot_count == 16)
            {
                target = 0;
                for (u64 candidate = 1; candidate < snapshot_count; candidate++)
                {
                    if (snapshot[candidate].header.sequence < snapshot[target].header.sequence)
                        target = candidate;
                }
                if (before <= snapshot[target].header.sequence)
                    continue;
            }

            snapshot[target] = ring[index];
            const u64 after = __atomic_load_n(&ring[index].header.sequence, __ATOMIC_ACQUIRE);
            if (before == after)
            {
                if (snapshot_count < 16)
                    snapshot_count++;
            }
        }
    }
    u64 last_sequence = 0;
    for (u64 emitted = 0; emitted < snapshot_count; emitted++)
    {
        u64 next_index = snapshot_count;
        u64 next_sequence = ~u64{0};
        for (u64 index = 0; index < snapshot_count; index++)
        {
            const u64 sequence = snapshot[index].header.sequence;
            if (sequence > last_sequence && sequence < next_sequence)
            {
                next_sequence = sequence;
                next_index = index;
            }
        }
        if (next_index == snapshot_count)
            break;
        const u64 snapshot_text_length = render_record(snapshot[next_index], buffer, max_rendered_record);
        write_panic(buffer, snapshot_text_length);
        last_sequence = next_sequence;
    }
    const u64 dropped_records = total_dropped.load(std::memory_order_relaxed);
    if (dropped_records != 0)
    {
        freelibcxx::format_detail::writer dropped_output{freelibcxx::span<char>(buffer, max_rendered_record - 1)};
        dropped_output.put("[log] dropped=");
        append_integer(dropped_output, dropped_records);
        dropped_output.put(" records before the oldest retained record");
        dropped_output.put('\n');
        write_panic(buffer, dropped_output.written);
    }
    for (u32 cpu_id = 0; cpu_id < max_cpu_count; cpu_id++)
    {
        const u16 length = nmi_buffer_lengths[cpu_id].load(std::memory_order_acquire);
        if (length != 0)
            write_panic(nmi_buffers[cpu_id], length);
    }
    const u64 dropped_nmi = nmi_dropped.load(std::memory_order_relaxed);
    if (dropped_nmi != 0)
    {
        freelibcxx::format_detail::writer dropped_output{freelibcxx::span<char>(buffer, max_rendered_record - 1)};
        dropped_output.put("[nmi] dropped=");
        append_integer(dropped_output, dropped_nmi);
        dropped_output.put('\n');
        write_panic(buffer, dropped_output.written);
    }
    const u64 panic_rendered = render_record(item, buffer, max_rendered_record);
    write_panic(buffer, panic_rendered);
    (void)regs;
}

void keep_panic(const regs_t *regs)
{
    print_stack(regs, 30);
    static constexpr char message[] = "[panic] kernel halted; connect GDB to inspect the saved state\n";
    write_panic(message, sizeof(message) - 1);
    for (;;)
        cpu_pause();
}
} // namespace log

ExportC NoReturn void panic_once(const char *string)
{
    log::detail::panic(log::module::kernel, "panic_once", 0, "{}", string);
}
