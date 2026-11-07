#include "invocation_handlers.hpp"
#include "invocation_region_support.hpp"
#include "kernel/arch/klib.hpp"
#include "kernel/errno.hpp"
#include "kernel/ipc/channel.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/log.hpp"
#include "kernel/mm/data_plane.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/mm/new.hpp"
#include "kernel/task.hpp"
#include "kernel/terminal.hpp"
#include "kernel/terminal_views.hpp"
#include "kernel/timer.hpp"
#include "kernel/ucontext.hpp"
#include "kernel/usercopy.hpp"
#include "naos/generated/system/MemoryObject.hpp"
#include "naos/generated/system/Process.hpp"
#include "naos/generated/system/Process_kernel_dispatch.hpp"
#include "naos/generated/system/TerminalJobControl.hpp"
#include "naos/generated/system_uapi.h"
#include <limits>

KLOG_MODULE(ipc);

namespace naos::ipc
{
namespace
{
using dispatch_detail::decode_message;
using dispatch_detail::empty_bytes;
using dispatch_detail::empty_resources;
using dispatch_detail::encode_message;
using dispatch_detail::kernel_view_metadata;
using dispatch_detail::request_region_object;
using dispatch_detail::status_sample_sequence;

void fill_process_status(naos::system::Process::ProcessStatus &encoded, const task::process_t *process)
{
    task::memory_accounting_t accounting;
    task::get_process_memory_accounting(process, accounting);
    encoded.sample_sequence = status_sample_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    encoded.sample_time_us = timer::get_high_resolution_time();
    encoded.page_size = memory::page_size;
    encoded.pid = process == nullptr ? 0 : process->pid;
    encoded.parent_pid = process == nullptr ? 0 : process->parent_pid;
    encoded.thread_count = accounting.thread_count;
    encoded.live_thread_count = accounting.live_thread_count;
    encoded.vma_count = accounting.vma_count;
    encoded.virtual_pages = accounting.virtual_pages;
    encoded.mapped_pages = accounting.mapped_pages;
    encoded.rss_pages = accounting.rss_pages;
    encoded.private_pages = accounting.private_pages;
    encoded.shared_pages = accounting.shared_pages;
    encoded.committed_pages = accounting.committed_pages;
    encoded.anonymous_pages = accounting.anonymous_pages;
    encoded.file_cache_pages = accounting.file_cache_pages;
    encoded.kernel_stack_pages = accounting.kernel_stack_pages;
    encoded.user_stack_pages = accounting.user_stack_pages;
    encoded.page_faults = accounting.page_faults;
    encoded.peak_rss_pages = accounting.peak_rss_pages;
    encoded.user_time_us = accounting.user_time_us;
    encoded.system_time_us = accounting.system_time_us;
}

void fill_thread_status(naos::system::Process::ThreadStatus &encoded, const task::thread_snapshot_entry &thread)
{
    encoded.tid = thread.tid;
    encoded.state = static_cast<u64>(thread.state);
    encoded.attributes = thread.attributes;
    encoded.cpu_id = thread.cpu_id;
    encoded.static_priority = thread.static_priority;
    encoded.dynamic_priority = thread.dynamic_priority;
    encoded.user_time_us = thread.user_time_us;
    encoded.system_time_us = thread.system_time_us;
    encoded.name_offset = 0;
    encoded.name_bytes = 0;
}

bool ensure_process_started(task::process_t *process)
{
    if (process == nullptr)
        return false;

    const auto state = process->start_state.load(std::memory_order_acquire);
    if (state == task::process_start_state::started)
        return true;
    if (state == task::process_start_state::aborted)
        return false;

    if (task::start_process(process))
        return true;

    // Concurrent duplicate Process.start calls race on the one-shot state
    // transition. A loser observes the winner's committed state as success.
    return process->start_state.load(std::memory_order_acquire) == task::process_start_state::started;
}

struct process_kernel_handler
{
    invocation_state &state;
    task::process_t *target;
    task::process_t *caller;

    na_status_t invalid_request() { return NA_STATUS_INVALID_MESSAGE; }

    template <typename Response>
    static void fill_wait_response(Response &response, i64 status, process_id pid, const task::statistics_t &statistics)
    {
        response.status = status;
        response.pid = pid;
        response.user_time_us = statistics.user_time;
        response.system_time_us = statistics.sys_time;
        response.page_faults = statistics.page_faults;
        response.peak_rss_pages = statistics.peak_rss_pages;
        response.page_size = memory::page_size;
        response.user_stack_pages = statistics.user_stack_pages;
        response.file_inputs = statistics.file_inputs;
        response.file_outputs = statistics.file_outputs;
        response.signals_delivered = statistics.signals_delivered;
        response.voluntary_context_switches = statistics.voluntary_context_switches;
        response.involuntary_context_switches = statistics.involuntary_context_switches;
        response.text_pages = statistics.text_pages;
        response.resident_pages = statistics.resident_pages;
    }

    kernel_dispatch::method_result wait(const naos::system::Process::wait_request &request,
                                        naos::system::Process::wait_response &response,
                                        const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        i64 status = 0;
        process_id waited_pid = 0;
        task::statistics_t statistics{};
        const auto result = static_cast<i64>(task::wait_process_handle(
            caller, target, static_cast<flag_t>(request.flags), status, waited_pid, statistics,
            [this] { return state.execution_interrupted(); },
            [this](task::wait_queue_t *queue) { state.set_execution_wait_queue(queue); }));
        if (state.execution_interrupted())
            return kernel_dispatch::method_result::interrupted();
        if (result != 0)
            return kernel_dispatch::method_result::error(result < 0 ? result : -result);
        fill_wait_response(response, status, waited_pid, statistics);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result wait_children(const naos::system::Process::wait_children_request &request,
                                                 naos::system::Process::wait_children_response &response,
                                                 const capability::transfer_record_list &,
                                                 capability::transfer_record_list &)
    {
        if (target != caller)
            return kernel_dispatch::method_result::error(EACCES);
        i64 status = 0;
        process_id waited_pid = 0;
        task::statistics_t statistics{};
        const auto result = task::wait_process_children(
            caller, request.pid, static_cast<flag_t>(request.flags), status, waited_pid, statistics,
            [this] { return state.execution_interrupted(); },
            [this](task::wait_queue_t *queue) { state.set_execution_wait_queue(queue); });
        if (state.execution_interrupted())
            return kernel_dispatch::method_result::interrupted();
        if (result != 0)
            return kernel_dispatch::method_result::error(result < 0 ? result : -result);
        fill_wait_response(response, status, waited_pid, statistics);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get_info(const naos::system::Process::get_info_request &,
                                            naos::system::Process::get_info_response &response,
                                            const capability::transfer_record_list &,
                                            capability::transfer_record_list &)
    {
        response.pid = target->pid;
        response.parent_pid = target->parent_pid;
        response.attributes = target->attributes.load();
        response.return_value = target->ret_val;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get_status(const naos::system::Process::get_status_request &request,
                                              naos::system::Process::get_status_response &response,
                                              capability::transfer_record_list &resources,
                                              capability::transfer_record_list &)
    {
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                  request.size, region_offset);
        if (region == nullptr || !region->writable(region_offset, request.size))
            return kernel_dispatch::method_result::error(EINVAL);
        fill_process_status(response.value, target);
        const auto status = task::read_process_name(target, *region, request.size, response.value.name_bytes,
                                                    response.value.name_required_bytes);
        if (status != NA_STATUS_OK && status != NA_STATUS_BUFFER_TOO_SMALL)
            return kernel_dispatch::method_result::error(status);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get_command_line(const naos::system::Process::get_command_line_request &request,
                                                    naos::system::Process::get_command_line_response &response,
                                                    capability::transfer_record_list &resources,
                                                    capability::transfer_record_list &)
    {
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                  request.size, region_offset);
        if (region == nullptr || !region->writable(region_offset, request.size))
            return kernel_dispatch::method_result::error(EINVAL);
        const auto status = task::read_process_command_line(target, *region, request.size, response.actual_bytes,
                                                            response.required_bytes);
        if (status != NA_STATUS_OK && status != NA_STATUS_BUFFER_TOO_SMALL)
            return kernel_dispatch::method_result::error(status);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result list_threads(const naos::system::Process::list_threads_request &request,
                                                naos::system::Process::list_threads_response &response,
                                                capability::transfer_record_list &resources,
                                                capability::transfer_record_list &)
    {
        u64 region_offset = 0;
        auto *region =
            request_region_object(resources, request.buffer.value, NA_MEMORY_RIGHT_MAP | NA_MEMORY_RIGHT_WRITE, 0,
                                  request.size, region_offset);
        if (region == nullptr || !region->writable(region_offset, request.size))
            return kernel_dispatch::method_result::error(EINVAL);

        freelibcxx::vector<task::thread_snapshot_entry> entries(memory::KernelCommonAllocatorV);
        thread_id next_tid = 0;
        const auto snapshot_status = task::open_thread_snapshot(target, request.after_tid, 64, entries, next_tid);
        if (snapshot_status != 0)
            return kernel_dispatch::method_result::error(snapshot_status);

        response.next_after_tid = next_tid;
        u64 required_name_bytes = 0;
        for (const auto &entry : entries)
        {
            const u64 entry_name_bytes = strlen(entry.name) + 1;
            if (required_name_bytes > ~u64(0) - entry_name_bytes)
                return kernel_dispatch::method_result::error(EOVERFLOW);
            required_name_bytes += entry_name_bytes;
        }
        response.name_required_bytes = required_name_bytes;
        const bool names_fit = request.size >= required_name_bytes;
        response.name_bytes = names_fit ? required_name_bytes : 0;
        response.threads.data = response.threads_storage.data();
        response.threads.count = static_cast<u32>(entries.size());
        u64 name_offset = 0;
        for (u32 index = 0; index < response.threads.count; index++)
        {
            fill_thread_status(response.threads.data[index], entries[index]);
            if (names_fit)
            {
                const u64 entry_name_bytes = strlen(entries[index].name) + 1;
                u64 written = 0;
                if (region->write(region_offset + name_offset, reinterpret_cast<const byte *>(entries[index].name),
                                  entry_name_bytes, written) != NA_STATUS_OK ||
                    written != entry_name_bytes)
                    return kernel_dispatch::method_result::error(EINVAL);
                response.threads.data[index].name_offset = name_offset;
                response.threads.data[index].name_bytes = entry_name_bytes;
                name_offset += entry_name_bytes;
            }
        }
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get_job_control_info(const naos::system::Process::get_job_control_info_request &,
                                                        naos::system::Process::get_job_control_info_response &response,
                                                        const capability::transfer_record_list &,
                                                        capability::transfer_record_list &)
    {
        task::job_control_info info{};
        if (!task::get_job_control_info(target, info))
            return kernel_dispatch::method_result::error(EINVAL);
        response.session = static_cast<i64>(info.session);
        response.process_group = static_cast<i64>(info.process_group);
        response.foreground_process_group = static_cast<i64>(info.foreground_process_group);
        response.has_controlling_tty = info.has_controlling_tty ? 1 : 0;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result
    get_controlling_terminal(const naos::system::Process::get_controlling_terminal_request &,
                             naos::system::Process::get_controlling_terminal_response &response,
                             const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        if (target != caller)
            return kernel_dispatch::method_result::error(EACCES);
        na_terminal_locator_t locator{};
        if (!task::get_controlling_terminal_locator(target, locator))
            return kernel_dispatch::method_result::error(ENXIO);
        response.terminal_id = locator.terminal_id;
        response.generation = locator.generation;
        for (u64 index = 0; index < response.token.size(); index++)
            response.token[index] = locator.token[index];
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result set_session(const naos::system::Process::set_session_request &,
                                               naos::system::Process::set_session_response &response,
                                               const capability::transfer_record_list &,
                                               capability::transfer_record_list &)
    {
        if (target != caller)
            return kernel_dispatch::method_result::error(EACCES);
        const auto session = task::setsid(target);
        if (session < 0)
            return kernel_dispatch::method_result::error(session);
        response.session = session;
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get_process_group(const naos::system::Process::get_process_group_request &,
                                                     naos::system::Process::get_process_group_response &response,
                                                     const capability::transfer_record_list &,
                                                     capability::transfer_record_list &)
    {
        task::job_control_info info{};
        if (!task::get_job_control_info(target, info))
            return kernel_dispatch::method_result::error(EINVAL);
        response.process_group = static_cast<i64>(info.process_group);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result get_session(const naos::system::Process::get_session_request &,
                                               naos::system::Process::get_session_response &response,
                                               const capability::transfer_record_list &,
                                               capability::transfer_record_list &)
    {
        task::job_control_info info{};
        if (!task::get_job_control_info(target, info))
            return kernel_dispatch::method_result::error(EINVAL);
        response.session = static_cast<i64>(info.session);
        return kernel_dispatch::method_result::success();
    }

    kernel_dispatch::method_result set_process_group(const naos::system::Process::set_process_group_request &request,
                                                     naos::system::Process::set_process_group_response &,
                                                     const capability::transfer_record_list &,
                                                     capability::transfer_record_list &)
    {
        if (request.process_group < 0)
            return kernel_dispatch::method_result::error(EINVAL);
        const auto result = task::setpgid(caller, target->pid, static_cast<group_id>(request.process_group));
        return result == 0 ? kernel_dispatch::method_result::success() : kernel_dispatch::method_result::error(result);
    }

    kernel_dispatch::method_result start(const naos::system::Process::start_request &,
                                         naos::system::Process::start_response &,
                                         const capability::transfer_record_list &, capability::transfer_record_list &)
    {
        if (caller == nullptr || target->parent_pid != caller->pid)
            return kernel_dispatch::method_result::error(EACCES);
        // Process.start is idempotent so a caller can safely retry when the
        // invocation reply is lost after the kernel committed the start.
        return ensure_process_started(target) ? kernel_dispatch::method_result::success()
                                              : kernel_dispatch::method_result::error(EBUSY);
    }
};

na_status_t publish_process_call(invocation_state &state, task::process_object &process, u64 method_id,
                                 const freelibcxx::vector<byte> &request, capability::transfer_record_list &resources,
                                 task::process_t *caller)
{
    auto *target = process.process();
    if (target == nullptr)
        return NA_STATUS_OBJECT_REVOKED;
    process_kernel_handler handler{state, target, caller};
    return naos::system::Process::ProcessKernelDispatcher::dispatch(handler, state, method_id, request, resources);
}

} // namespace

namespace
{
na_status_t dispatch_process_object(kernel_dispatch::kernel_view_context &context, task::process_object &process)
{
    return publish_process_call(context.state, process, context.method_id, context.request, context.resources,
                                context.caller);
}

constexpr kernel_dispatch::kernel_view_route process_kernel_view_route_entries[] = {
    kernel_dispatch::make_kernel_view_route<task::process_object, dispatch_process_object>(
        NA_SCOPE_PROCESS, naos::system::Process::descriptor),
};
} // namespace

kernel_dispatch::kernel_view_route_table process_kernel_view_routes() noexcept
{
    return {process_kernel_view_route_entries, 1};
}
} // namespace naos::ipc
