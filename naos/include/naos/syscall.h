#ifndef NAOS_SYSCALL_H
#define NAOS_SYSCALL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <naos/abi.h>

#if defined(__cplusplus)
#define NAOS_SYSCALL_NORETURN [[noreturn]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define NAOS_SYSCALL_NORETURN _Noreturn
#else
#define NAOS_SYSCALL_NORETURN
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct na_time_clock
{
    int64_t tv_sec;
    int64_t tv_nsec;
} na_time_clock_t;

typedef struct na_signal_info
{
    int64_t error;
    int64_t code;
    int64_t status;
    int64_t pid;
    int64_t tid;
} na_signal_info_t;

typedef struct na_signal_target
{
    int64_t id;
    int64_t flags;
} na_signal_target_t;

typedef uint64_t na_signal_mask_t;

/* Legacy process and signal syscall wrappers. */
void _s_none(void);
void _s_log(const char *message);
int _s_clock(int clock_index, na_time_clock_t *clock);
int _s_futex(int *ptr, int op, int val, const na_time_clock_t *timeout);
NAOS_SYSCALL_NORETURN void _s_exit(int64_t ret);
NAOS_SYSCALL_NORETURN void _s_exit_thread(int64_t ret);
int _s_sleep(const na_time_clock_t *time);
int64_t _s_current_pid(void);
int64_t _s_current_tid(void);
int _s_sigsend(na_signal_target_t *target, int signum, na_signal_info_t *info);
int _s_sigmask(int opt, na_signal_mask_t *valid, na_signal_mask_t *block, na_signal_mask_t *ignore);
int _s_tcb_set(void *p);
int _s_fork(void);
int _s_clone(void *entry, void *arg, void *tcb);
int _s_yield(void);
bool _s_brk(uint64_t ptr);
uint64_t _s_sbrk(int64_t offset);

/* Native capability and invocation syscall wrappers. Every native syscall
 * returns na_status_t; successful output values use frames or out parameters. */
na_status_t _na_handle_close(na_handle_t handle);
na_status_t _na_channel_create(const na_channel_options_t *options, na_handle_t *left, na_handle_t *right);
na_status_t _na_channel_send(na_handle_t endpoint, const na_channel_send_frame_t *frame);
na_status_t _na_channel_receive(na_handle_t endpoint, na_channel_receive_frame_t *frame);
na_status_t _na_channel_discard(na_handle_t endpoint);
na_status_t _na_handle_wait_many(na_wait_item_t *items, uint64_t count, const struct timespec *deadline);
na_status_t _na_handle_duplicate(na_handle_t source, na_meta_rights_t rights, na_handle_t *result);
na_status_t _na_handle_restrict(na_handle_t source, const na_handle_restriction_t *restriction, na_handle_t *result);
na_status_t _na_handle_get_info(na_handle_t handle, na_handle_info_t *result);
na_status_t _na_protocol_descriptor_create(const na_protocol_descriptor_t *input, na_handle_t *result);
na_status_t _na_protocol_endpoint_create(na_handle_t descriptor, const na_protocol_endpoint_options_t *options,
                                         na_handle_t *client, na_handle_t *server);
na_status_t _na_invoke_submit(na_handle_t target, const na_submit_frame_t *frame, na_handle_t *invocation);
na_status_t _na_invoke_oneway(na_handle_t target, const na_submit_frame_t *frame);
na_status_t _na_invocation_cancel(na_handle_t invocation);
na_status_t _na_invocation_take_result(na_handle_t invocation, na_result_frame_t *frame);
na_status_t _na_responder_reply(na_handle_t responder, const na_reply_frame_t *frame);
na_status_t _na_responder_fail(na_handle_t responder, const na_fail_frame_t *frame);
na_status_t _na_bootstrap(na_bootstrap_frame_t *frame);
na_status_t _na_memory_map(na_memory_map_frame_t *frame);
na_status_t _na_memory_unmap(na_memory_unmap_frame_t *frame);
na_status_t _na_process_exec(const na_process_exec_frame_t *frame);
na_status_t _na_process_handle_open(int64_t pid, na_handle_t *result);
na_status_t _na_process_spawn(const na_process_spawn_frame_t *frame);
na_status_t _na_pipe_create(na_pipe_create_frame_t *frame);

#ifdef __cplusplus
}
#endif

#undef NAOS_SYSCALL_NORETURN

#endif
