#pragma once

#include "freelibcxx/formatter.hpp"
#include "kernel/arch/regs.hpp"
#include "kernel/common.hpp"
#include <atomic>
#include <type_traits>

namespace log
{
enum class level : u8
{
    trace,
    debug,
    info,
    warning,
    error,
    panic,
};

constexpr bool record_has_metadata_prefix(level value) noexcept { return value != level::panic; }

enum class module : u8
{
    kernel,
    arch,
    acpi,
    mm,
    task,
    sched,
    ipc,
    fs,
    dev,
    io,
    tty,
    test,
};

enum record_flags : u8
{
    early_record = 1 << 0,
    message_truncated = 1 << 1,
    raw_record = 1 << 2,
    file_truncated = 1 << 3,
    unknown_context = 1 << 4,
    nmi_record = 1 << 5,
    user_record = 1 << 6,
};

constexpr u16 max_message_length = 512;
constexpr u8 max_file_length = 95;
constexpr u8 max_process_name_length = 12;

struct record_header
{
    u64 sequence = 0;
    u64 timestamp = 0;
    u32 cpu_id = 0;
    u32 pid = 0;
    u32 tid = 0;
    u32 line = 0;
    u16 message_length = 0;
    u8 level = 0;
    u8 flags = 0;
    u8 module = 0;
    u8 file_length = 0;
};

struct record
{
    record_header header;
    char file[max_file_length + 1]{};
    char process_name[max_process_name_length + 1]{};
    char message[max_message_length + 1]{};
};

struct sink_config
{
    bool enabled = false;
    level minimum = level::info;
    bool color = false;
};

struct configuration
{
    level retention_level = level::info;
    sink_config console{true, level::info, true};
    sink_config serial{true, level::debug, false};
    sink_config dmesg{true, level::info, true};
    bool emergency_serial = true;
};

const char *level_name(level value);
const char *module_name(module value);
bool enabled(level value, module source);
const configuration &config();

void early_init();
void init();
void start_workers();
void flush_sinks();
NoReturn void keep_panic(const regs_t *regs = nullptr);

template <typename T> void *hex(T value)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<void *>(value);
    else
        return reinterpret_cast<void *>(static_cast<uintptr_t>(value));
}

namespace detail
{
void emit_message(level severity, module source, const char *file, u32 line, bool raw, const char *message, u64 length);
void emit_user_message(const char *process_name, const char *file, u32 line, bool raw, const char *message, u64 length);
void emit_nmi_message(module source, const char *file, u32 line, const char *message, u64 length);
void emit_panic_message(module source, const char *file, u32 line, bool raw, const char *message, u64 length,
                        const regs_t *regs);

template <typename... Args>
void emit(level severity, module source, const char *file, u32 line, bool raw,
          freelibcxx::format_string<std::type_identity_t<Args>...> format, const Args &...args)
{
    char message[max_message_length + 1]{};
    const auto result = freelibcxx::format_to(freelibcxx::span<char>(message, max_message_length), format, args...);
    emit_message(severity, source, file, line, raw, message, result.written);
}

template <typename... Args>
NoReturn void panic(module source, const char *file, u32 line,
                    freelibcxx::format_string<std::type_identity_t<Args>...> format, const Args &...args)
{
    char message[max_message_length + 1]{};
    const auto result = freelibcxx::format_to(freelibcxx::span<char>(message, max_message_length), format, args...);
    emit_panic_message(source, file, line, false, message, result.written, nullptr);
    keep_panic();
}

template <typename... Args>
NoReturn void panic_stack(module source, const char *file, u32 line, const regs_t *regs,
                          freelibcxx::format_string<std::type_identity_t<Args>...> format, const Args &...args)
{
    char message[max_message_length + 1]{};
    const auto result = freelibcxx::format_to(freelibcxx::span<char>(message, max_message_length), format, args...);
    emit_panic_message(source, file, line, false, message, result.written, regs);
    keep_panic(regs);
}

template <typename... Args>
void raw(module source, const char *file, u32 line, freelibcxx::format_string<std::type_identity_t<Args>...> format,
         const Args &...args)
{
    char message[max_message_length + 1]{};
    const auto result = freelibcxx::format_to(freelibcxx::span<char>(message, max_message_length), format, args...);
    emit_message(level::info, source, file, line, true, message, result.written);
}

template <typename... Args>
void nmi(module source, const char *file, u32 line, freelibcxx::format_string<std::type_identity_t<Args>...> format,
         const Args &...args)
{
    char message[max_message_length + 1]{};
    const auto result = freelibcxx::format_to(freelibcxx::span<char>(message, max_message_length), format, args...);
    emit_nmi_message(source, file, line, message, result.written);
}

template <typename... Args>
void assert_runtime(const char *expression, const char *file, u32 line,
                    freelibcxx::format_string<std::type_identity_t<Args>...> format, const Args &...args)
{
    char message[max_message_length + 1]{};
    freelibcxx::format_detail::writer output{freelibcxx::span<char>(message, max_message_length)};
    output.put("assertion failed: ");
    output.put(expression);
    output.put(' ');
    const auto result = freelibcxx::format_to(
        freelibcxx::span<char>(message + output.written, max_message_length - output.written), format, args...);
    output.written += result.written;
    raw(module::kernel, file, line, "{}", message);
    panic(module::kernel, file, line, "from assert failed");
}
} // namespace detail
} // namespace log

#define KLOG_MODULE(name) constexpr ::log::module klog_module = ::log::module::name

#define KLOG_TRACE(...)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::log::enabled(::log::level::trace, klog_module))                                                          \
            ::log::detail::emit(::log::level::trace, klog_module, __FILE__, __LINE__, false, __VA_ARGS__);             \
    } while (0)

#define KLOG_DEBUG(...)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::log::enabled(::log::level::debug, klog_module))                                                          \
            ::log::detail::emit(::log::level::debug, klog_module, __FILE__, __LINE__, false, __VA_ARGS__);             \
    } while (0)

#define KLOG_INFO(...)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::log::enabled(::log::level::info, klog_module))                                                           \
            ::log::detail::emit(::log::level::info, klog_module, __FILE__, __LINE__, false, __VA_ARGS__);              \
    } while (0)

#define KLOG_WARN(...)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::log::enabled(::log::level::warning, klog_module))                                                        \
            ::log::detail::emit(::log::level::warning, klog_module, __FILE__, __LINE__, false, __VA_ARGS__);           \
    } while (0)

#define KLOG_ERROR(...)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::log::enabled(::log::level::error, klog_module))                                                          \
            ::log::detail::emit(::log::level::error, klog_module, __FILE__, __LINE__, false, __VA_ARGS__);             \
    } while (0)

#define KLOG_PANIC(...) ::log::detail::panic(klog_module, __FILE__, __LINE__, __VA_ARGS__)

#define KLOG_RAW(...)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::log::enabled(::log::level::info, klog_module))                                                           \
            ::log::detail::raw(klog_module, __FILE__, __LINE__, __VA_ARGS__);                                          \
    } while (0)

#define KLOG_NMI(...)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::log::enabled(::log::level::debug, klog_module))                                                          \
            ::log::detail::nmi(klog_module, __FILE__, __LINE__, __VA_ARGS__);                                          \
    } while (0)

#define KLOG_PANIC_STACK(regs, ...) ::log::detail::panic_stack(klog_module, __FILE__, __LINE__, regs, __VA_ARGS__)

#ifdef _DEBUG
#define kassert(exp, ...)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (unlikely(!(exp)))                                                                                          \
            ::log::detail::assert_runtime(#exp, __FILE__, __LINE__, __VA_ARGS__);                                      \
    } while (0)
#else
#define kassert(exp, ...) void(0)
#endif

extern "C" NoReturn void panic_once(const char *string);
