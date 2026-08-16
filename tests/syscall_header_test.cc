#include <type_traits>

#include <naos/syscall.h>

static_assert(std::is_same_v<decltype(&_s_log), void (*)(const char *)>);
static_assert(std::is_same_v<decltype(&_s_clock), int (*)(int, na_time_clock_t *)>);
static_assert(std::is_same_v<decltype(&_s_futex), int (*)(int *, int, int, const na_time_clock_t *)>);
static_assert(std::is_same_v<decltype(&_s_sigsend), int (*)(na_signal_target_t *, int, na_signal_info_t *)>);
static_assert(
    std::is_same_v<decltype(&_s_sigmask), int (*)(int, na_signal_mask_t *, na_signal_mask_t *, na_signal_mask_t *)>);
static_assert(std::is_same_v<decltype(&_na_handle_close), na_status_t (*)(na_handle_t)>);
static_assert(std::is_same_v<decltype(&_na_channel_create),
                             na_status_t (*)(const na_channel_options_t *, na_handle_t *, na_handle_t *)>);
static_assert(
    std::is_same_v<decltype(&_na_channel_send), na_status_t (*)(na_handle_t, const na_channel_send_frame_t *)>);
static_assert(
    std::is_same_v<decltype(&_na_channel_receive), na_status_t (*)(na_handle_t, na_channel_receive_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_channel_discard), na_status_t (*)(na_handle_t)>);
static_assert(std::is_same_v<decltype(&_na_handle_wait_many),
                             na_status_t (*)(na_wait_item_t *, uint64_t, const struct timespec *)>);
static_assert(
    std::is_same_v<decltype(&_na_handle_duplicate), na_status_t (*)(na_handle_t, na_meta_rights_t, na_handle_t *)>);
static_assert(std::is_same_v<decltype(&_na_handle_restrict),
                             na_status_t (*)(na_handle_t, const na_handle_restriction_t *, na_handle_t *)>);
static_assert(std::is_same_v<decltype(&_na_handle_get_info), na_status_t (*)(na_handle_t, na_handle_info_t *)>);
static_assert(std::is_same_v<decltype(&_na_protocol_descriptor_create),
                             na_status_t (*)(const na_protocol_descriptor_t *, na_handle_t *)>);
static_assert(
    std::is_same_v<decltype(&_na_protocol_endpoint_create),
                   na_status_t (*)(na_handle_t, const na_protocol_endpoint_options_t *, na_handle_t *, na_handle_t *)>);
static_assert(std::is_same_v<decltype(&_na_invoke_submit),
                             na_status_t (*)(na_handle_t, const na_submit_frame_t *, na_handle_t *)>);
static_assert(std::is_same_v<decltype(&_na_invoke_oneway), na_status_t (*)(na_handle_t, const na_submit_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_invocation_cancel), na_status_t (*)(na_handle_t)>);
static_assert(std::is_same_v<decltype(&_na_invocation_take_result), na_status_t (*)(na_handle_t, na_result_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_responder_reply), na_status_t (*)(na_handle_t, const na_reply_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_responder_fail), na_status_t (*)(na_handle_t, const na_fail_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_bootstrap), na_status_t (*)(na_bootstrap_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_process_exec), na_status_t (*)(const na_process_exec_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_process_handle_open), na_status_t (*)(int64_t, na_handle_t *)>);
static_assert(std::is_same_v<decltype(&_na_process_spawn), na_status_t (*)(const na_process_spawn_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_pipe_create), na_status_t (*)(na_pipe_create_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_memory_map), na_status_t (*)(na_memory_map_frame_t *)>);
static_assert(std::is_same_v<decltype(&_na_memory_unmap), na_status_t (*)(na_memory_unmap_frame_t *)>);

int run_syscall_header_tests() { return 0; }
