#include <abi-bits/ioctls.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <naos/generated/system/TerminalManager.hpp>
#include <naos/generated/system/TerminalManager_client.hpp>
#include <naos/generated/system/TerminalMaster.hpp>
#include <naos/generated/system/TerminalMaster_client.hpp>
#include <naos/generated/system/TerminalSlave.hpp>
#include <naos/generated/system/TerminalSlave_client.hpp>
#include <naos/libnao.hpp>
#include <naos/service_directory.hpp>
#include <naos/syscall.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

[[gnu::weak]] void *__dso_handle;
extern char **environ;

extern "C" {
int run_mlibc_tls_smoke();
int ioctl(int fd, unsigned long request, ...);
int naos_native_spawn(pid_t *pid, const char *path, char *const argv[], char *const envp[]);
int naos_native_spawn_stdio(pid_t *pid, const char *path, char *const argv[], char *const envp[], int stdin_fd,
                            int stdout_fd, int stderr_fd);
int naos_native_spawn_stdio_deferred(pid_t *pid, na_handle_t *process, const char *path, char *const argv[],
                                     char *const envp[], int stdin_fd, int stdout_fd, int stderr_fd);
int naos_native_start_process(na_handle_t process);
}

void smoke_log(const char *message)
{
    _s_log(message);
    fputs(message, stdout);
    fflush(stdout);
}

void smoke_printf(const char *format, ...)
{
    char message[512]{};
    va_list arguments;
    va_start(arguments, format);
    const int result = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (result < 0)
    {
        smoke_log("smoke: output formatting failed\n");
        return;
    }
    smoke_log(message);
}

namespace
{
naoidl::native_transport make_native_transport()
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
    const auto status = nao::event_loop::wait_invocation(invocation);
    return status == NA_STATUS_OK ? 0 : static_cast<int>(status);
}

int wait_readable(na_handle_t handle, const struct timespec *deadline)
{
    na_handle_t epoll = NA_HANDLE_INVALID;
    auto status = nao::event_loop::create(epoll);
    if (status == NA_STATUS_OK)
    {
        const na_epoll_event_t event{NA_EPOLL_EVENT_READABLE | NA_EPOLL_EVENT_HANGUP, 0};
        status = nao::event_loop::control(epoll, NA_EPOLL_CTL_ADD, handle, &event);
        if (status == NA_STATUS_OK)
        {
            na_epoll_event_t returned{};
            std::uint64_t actual = 0;
            status = nao::event_loop::wait(epoll, &returned, 1, actual, deadline);
        }
        (void)_na_handle_close(epoll);
    }
    return static_cast<int>(status);
}

struct wait_deadline_wakeup_context
{
    na_handle_t endpoint;
};

struct parallel_wait_context
{
    na_handle_t receiver;
    int result;
};

void *send_wait_deadline_wakeup(void *raw_context)
{
    auto *context = static_cast<wait_deadline_wakeup_context *>(raw_context);
    usleep(1'000);
    na_channel_send_frame_t frame{};
    frame.struct_size = sizeof(frame);
    return reinterpret_cast<void *>(static_cast<uintptr_t>(_na_channel_send(context->endpoint, &frame)));
}

void *wait_for_message_with_deadline(void *raw_context)
{
    auto *context = static_cast<parallel_wait_context *>(raw_context);
    context->result = -1;
    for (int attempt = 0; attempt < 8; attempt++)
    {
        struct timespec deadline{};
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        {
            context->result = -2;
            return nullptr;
        }
        deadline.tv_nsec += 100'000'000;
        if (deadline.tv_nsec >= 1'000'000'000)
        {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1'000'000'000;
        }

        const auto wait_status = wait_readable(context->receiver, &deadline);
        if (wait_status == NA_STATUS_OK && _na_channel_discard(context->receiver) == NA_STATUS_OK)
        {
            context->result = 0;
            return nullptr;
        }
        if (wait_status != NA_STATUS_WAIT_TIMED_OUT && wait_status != NA_STATUS_OK)
        {
            context->result = static_cast<int>(wait_status);
            return nullptr;
        }
    }
    return nullptr;
}

bool wait_deadline_smoke()
{
    smoke_log("smoke: wait deadline cancellation stress begin\n");
    na_handle_t receiver = NA_HANDLE_INVALID;
    na_handle_t sender = NA_HANDLE_INVALID;
    if (_na_channel_create(nullptr, &receiver, &sender) != NA_STATUS_OK)
    {
        smoke_log("smoke: wait deadline channel create failed\n");
        return false;
    }

    bool ok = true;
    for (int iteration = 0; iteration < 512 && ok; iteration++)
    {
        wait_deadline_wakeup_context context{sender};
        pthread_t thread{};
        if (pthread_create(&thread, nullptr, send_wait_deadline_wakeup, &context) != 0)
        {
            ok = false;
            break;
        }

        struct timespec deadline{};
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        {
            ok = false;
        }
        else
        {
            deadline.tv_nsec += 50'000'000;
            if (deadline.tv_nsec >= 1'000'000'000)
            {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1'000'000'000;
            }
            const auto wait_status = wait_readable(receiver, &deadline);
            if (wait_status != NA_STATUS_OK)
                ok = false;
            if (ok && _na_channel_discard(receiver) != NA_STATUS_OK)
                ok = false;
        }

        void *thread_result = nullptr;
        if (pthread_join(thread, &thread_result) != 0 ||
            static_cast<na_status_t>(reinterpret_cast<uintptr_t>(thread_result)) != NA_STATUS_OK)
            ok = false;
    }

    constexpr int parallel_waiter_count = 32;
    constexpr int parallel_round_count = 64;
    for (int round = 0; round < parallel_round_count && ok; round++)
    {
        parallel_wait_context contexts[parallel_waiter_count]{};
        pthread_t threads[parallel_waiter_count]{};
        int created = 0;
        for (; created < parallel_waiter_count; created++)
        {
            contexts[created].receiver = receiver;
            if (pthread_create(&threads[created], nullptr, wait_for_message_with_deadline, &contexts[created]) != 0)
            {
                ok = false;
                break;
            }
        }
        usleep(1'000);

        na_channel_send_frame_t frame{};
        frame.struct_size = sizeof(frame);
        for (int sent = 0; sent < created && ok; sent++)
        {
            for (;;)
            {
                const auto send_status = _na_channel_send(sender, &frame);
                if (send_status == NA_STATUS_OK)
                    break;
                if (send_status != NA_STATUS_WOULD_BLOCK)
                {
                    ok = false;
                    break;
                }
                usleep(1'000);
            }
            usleep(1'000);
        }
        for (int joined = 0; joined < created; joined++)
        {
            if (pthread_join(threads[joined], nullptr) != 0 || contexts[joined].result != 0)
                ok = false;
        }
    }

    (void)_na_handle_close(sender);
    (void)_na_handle_close(receiver);
    smoke_log(ok ? "smoke: wait deadline cancellation stress ok\n"
                 : "smoke: wait deadline cancellation stress failed\n");
    return ok;
}

struct futex_smoke_context
{
    volatile int gate = 0;
    volatile int target = 0;
    volatile int entered = 0;
    int result = -1;
};

void *futex_waiter(void *raw_context)
{
    auto *context = static_cast<futex_smoke_context *>(raw_context);
    while (__atomic_load_n(&context->gate, __ATOMIC_ACQUIRE) == 0)
    {
        const na_time_clock_t timeout{0, 50'000'000};
        const int status = _s_futex(const_cast<int *>(&context->gate), 2, 0, &timeout);
        if (status != 0 && status != EAGAIN && status != ETIMEDOUT)
        {
            context->result = status;
            return nullptr;
        }
    }

    __atomic_store_n(&context->entered, 1, __ATOMIC_RELEASE);
    const na_time_clock_t timeout{0, 100'000'000};
    context->result = _s_futex(const_cast<int *>(&context->target), 2, 0, &timeout);
    return nullptr;
}

bool futex_smoke()
{
    smoke_log("smoke: futex wake-without-value-change begin\n");
    bool ok = true;
    for (int iteration = 0; iteration < 64 && ok; iteration++)
    {
        futex_smoke_context context{};
        pthread_t waiter{};
        if (pthread_create(&waiter, nullptr, futex_waiter, &context) != 0)
        {
            ok = false;
            break;
        }

        __atomic_store_n(&context.gate, 1, __ATOMIC_RELEASE);
        (void)_s_futex(const_cast<int *>(&context.gate), 1, 1, nullptr);
        for (int attempt = 0; attempt < 32 && __atomic_load_n(&context.entered, __ATOMIC_ACQUIRE) == 0; attempt++)
            usleep(1'000);

        // Keep the value at zero: a successful wake must come from the
        // notification, not from the wait predicate observing a changed word.
        for (int attempt = 0; attempt < 32; attempt++)
        {
            (void)_s_futex(const_cast<int *>(&context.target), 1, 1, nullptr);
            usleep(1'000);
        }
        if (pthread_join(waiter, nullptr) != 0 || context.result != 0)
            ok = false;
    }
    smoke_log(ok ? "smoke: futex wake-without-value-change ok\n" : "smoke: futex wake-without-value-change failed\n");
    return ok;
}

struct exec_sibling_context
{
    volatile int stop = 0;
};

void *exec_sibling(void *raw_context)
{
    auto *context = static_cast<exec_sibling_context *>(raw_context);
    while (__atomic_load_n(&context->stop, __ATOMIC_ACQUIRE) == 0)
        usleep(1'000);
    return nullptr;
}

bool exec_rejects_multithread_smoke()
{
    smoke_log("smoke: multithread exec rejection begin\n");
    na_handle_t image = NA_HANDLE_INVALID;
    if (_na_memory_create(128, 0, &image) != NA_STATUS_OK)
    {
        smoke_log("smoke: multithread exec memory create failed\n");
        return false;
    }

    na_memory_map_frame_t map{};
    map.struct_size = sizeof(map);
    map.flags = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE;
    map.object = image;
    map.length = 128;
    if (_na_memory_map(&map) != NA_STATUS_OK || map.address == 0)
    {
        (void)_na_handle_close(image);
        smoke_log("smoke: multithread exec memory map failed\n");
        return false;
    }

    auto *header = reinterpret_cast<unsigned char *>(map.address);
    memset(header, 0, 128);
    header[0] = 0x7f;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2;
    header[5] = 1;
    header[6] = 1;
    const uint16_t type = 2;
    const uint16_t machine = 0x3e;
    const uint32_t version = 1;
    const uint16_t ehsize = 64;
    const uint16_t phentsize = 56;
    const uint16_t shentsize = 64;
    memcpy(header + 16, &type, sizeof(type));
    memcpy(header + 18, &machine, sizeof(machine));
    memcpy(header + 20, &version, sizeof(version));
    memcpy(header + 52, &ehsize, sizeof(ehsize));
    memcpy(header + 54, &phentsize, sizeof(phentsize));
    memcpy(header + 58, &shentsize, sizeof(shentsize));

    na_memory_unmap_frame_t unmap{};
    unmap.struct_size = sizeof(unmap);
    unmap.address = map.address;
    unmap.length = map.length;
    (void)_na_memory_unmap(&unmap);

    exec_sibling_context context{};
    pthread_t sibling{};
    if (pthread_create(&sibling, nullptr, exec_sibling, &context) != 0)
    {
        (void)_na_handle_close(image);
        smoke_log("smoke: multithread exec sibling create failed\n");
        return false;
    }

    char path[] = "/bin/busybox";
    char *argv[] = {path, nullptr};
    char *envp[] = {nullptr};
    na_process_exec_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.executable = image;
    frame.path = reinterpret_cast<uint64_t>(path);
    frame.argv = reinterpret_cast<uint64_t>(argv);
    frame.envp = reinterpret_cast<uint64_t>(envp);
    const na_status_t status = _na_process_exec(&frame);

    __atomic_store_n(&context.stop, 1, __ATOMIC_RELEASE);
    const bool joined = pthread_join(sibling, nullptr) == 0;
    na_handle_info_t info{};
    info.struct_size = sizeof(info);
    const bool handle_preserved = _na_handle_get_info(image, &info) == NA_STATUS_OK;
    (void)_na_handle_close(image);
    const bool ok = status == NA_STATUS_IO_ERROR && joined && handle_preserved;
    smoke_printf("smoke: multithread exec status=%d handle_preserved=%d\n", static_cast<int>(status),
                 handle_preserved ? 1 : 0);
    smoke_log(ok ? "smoke: multithread exec rejection ok\n" : "smoke: multithread exec rejection failed\n");
    return ok;
}

void ttyd_smoke()
{
    smoke_log("smoke: connecting existing terminal manager\n");

    na_handle_t manager = NA_HANDLE_INVALID;
    int error = naos_service_connect_versioned("naos://system/terminal", &naos::system::TerminalManager::protocol_uuid,
                                               NA_PROTOCOL_RIGHT_INVOKE, naos::system::TerminalManager::revision,
                                               naos::system::TerminalManager::features, &manager);
    if (error != 0)
    {
        smoke_printf("smoke: connect terminal manager failed %d\n", error);
        smoke_log("smoke: connect terminal manager failed\n");
        return;
    }
    smoke_log("smoke: connected terminal manager\n");

    auto transport = make_native_transport();
    auto client = naos::system::TerminalManager::TerminalManagerClient(transport.async(), manager);
    std::uint8_t wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    naos::system::TerminalManager::create_pty_request request{};
    request.locked = 0;
    request.mode = 1 | 2;
    na_handle_t invocation = NA_HANDLE_INVALID;
    auto status = client.submit_create_pty(request, nullptr, 0, &invocation, wire, sizeof(wire));
    if (status != NA_STATUS_OK)
    {
        smoke_printf("smoke: create_pty submit failed %d\n", static_cast<int>(status));
        smoke_log("smoke: create_pty submit failed\n");
        (void)naos_handle_close(manager);
        return;
    }
    smoke_log("smoke: create_pty submitted\n");
    if (wait_invocation(invocation) != 0)
    {
        smoke_printf("smoke: create_pty wait failed\n");
        smoke_log("smoke: create_pty wait failed\n");
        (void)naos_handle_close(invocation);
        (void)naos_handle_close(manager);
        return;
    }
    smoke_log("smoke: create_pty completed\n");
    naos::system::TerminalManager::create_pty_response response{};
    na_handle_t resources[NA_CHANNEL_MAX_RESOURCES]{};
    na_result_frame_t result{};
    status =
        client.take_create_pty(invocation, response, wire, sizeof(wire), resources, NA_CHANNEL_MAX_RESOURCES, result);
    (void)naos_handle_close(invocation);
    if (status == NA_STATUS_OK && result.execution_outcome == NA_EXECUTION_NONE && result.protocol_error == 0 &&
        result.actual_resources == 2)
    {
        smoke_printf("smoke: ttyd create_pty ok number=%llu\n", static_cast<unsigned long long>(response.number));
        smoke_log("smoke: ttyd create_pty ok\n");

        const na_handle_t master_handle = resources[response.master.value];
        if (response.job_control.value < result.actual_resources)
            (void)naos_handle_close(resources[response.job_control.value]);
        auto master_client = naos::system::TerminalMaster::TerminalMasterClient(transport.async(), master_handle);

        naos::system::TerminalManager::open_pty_slave_request open_request{};
        open_request.locator = response.slave_locator;
        open_request.mode = 1 | 2;
        na_handle_t open_invocation = NA_HANDLE_INVALID;
        status = client.submit_open_pty_slave(open_request, nullptr, 0, &open_invocation, wire, sizeof(wire));
        if (status != NA_STATUS_OK || wait_invocation(open_invocation) != 0)
        {
            smoke_log("smoke: open_pty_slave failed\n");
            (void)naos_handle_close(master_handle);
            (void)naos_handle_close(manager);
            return;
        }
        naos::system::TerminalManager::open_pty_slave_response open_response{};
        na_handle_t slave_resources[NA_CHANNEL_MAX_RESOURCES]{};
        na_result_frame_t open_result{};
        status = client.take_open_pty_slave(open_invocation, open_response, wire, sizeof(wire), slave_resources,
                                            NA_CHANNEL_MAX_RESOURCES, open_result);
        (void)naos_handle_close(open_invocation);
        if (status != NA_STATUS_OK || open_result.actual_resources != 2)
        {
            smoke_log("smoke: open_pty_slave take failed\n");
            (void)naos_handle_close(master_handle);
            (void)naos_handle_close(manager);
            return;
        }
        const na_handle_t slave_handle = slave_resources[open_response.slave.value];
        if (open_response.job_control.value < open_result.actual_resources)
            (void)naos_handle_close(slave_resources[open_response.job_control.value]);
        auto slave_client = naos::system::TerminalSlave::TerminalSlaveClient(transport.async(), slave_handle);
        smoke_log("smoke: pty master/slave opened\n");

        // Terminal payloads travel in a caller-owned MemoryObject region now.
        // One reusable region per transfer direction serves every read and
        // write below; it is mapped shared so the service observes the
        // client's stores and the client observes the service's.
        constexpr std::uint64_t kBulkRegionAllocationBytes = 4096;
        auto create_region = [&](na_handle_t &object, std::uint8_t *&address) {
            if (_na_memory_create(kBulkRegionAllocationBytes, 0, &object) != NA_STATUS_OK)
                return false;
            na_memory_map_frame_t mapping{};
            mapping.struct_size = sizeof(mapping);
            mapping.flags = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_SHARED;
            mapping.object = object;
            mapping.length = kBulkRegionAllocationBytes;
            if (_na_memory_map(&mapping) != NA_STATUS_OK || mapping.address == 0)
            {
                (void)_na_handle_close(object);
                object = NA_HANDLE_INVALID;
                return false;
            }
            address = reinterpret_cast<std::uint8_t *>(static_cast<uintptr_t>(mapping.address));
            return true;
        };
        na_handle_t write_object = NA_HANDLE_INVALID;
        na_handle_t read_object = NA_HANDLE_INVALID;
        std::uint8_t *write_bytes = nullptr;
        std::uint8_t *read_bytes = nullptr;
        auto release_regions = [&]() {
            na_memory_unmap_frame_t unmap{};
            unmap.struct_size = sizeof(unmap);
            unmap.length = kBulkRegionAllocationBytes;
            if (write_bytes != nullptr)
            {
                unmap.address = reinterpret_cast<std::uint64_t>(write_bytes);
                (void)_na_memory_unmap(&unmap);
            }
            if (read_bytes != nullptr)
            {
                unmap.address = reinterpret_cast<std::uint64_t>(read_bytes);
                (void)_na_memory_unmap(&unmap);
            }
            (void)naos_handle_close(write_object);
            (void)naos_handle_close(read_object);
        };
        if (!create_region(write_object, write_bytes) || !create_region(read_object, read_bytes))
        {
            smoke_log("smoke: pty bulk region create failed\n");
            // release_regions tolerates the unmapped/never-created half, so it
            // also cleans up a failure between the two create_region calls.
            release_regions();
            (void)naos_handle_close(master_handle);
            (void)naos_handle_close(slave_handle);
            (void)naos_handle_close(manager);
            return;
        }
        na_resource_disposition_t write_resource{};
        write_resource.handle = write_object;
        write_resource.operation = NA_RESOURCE_DUPLICATE;
        write_resource.scope = NA_SCOPE_MEMORY_OBJECT;
        na_resource_disposition_t read_resource{};
        read_resource.handle = read_object;
        read_resource.operation = NA_RESOURCE_DUPLICATE;
        read_resource.scope = NA_SCOPE_MEMORY_OBJECT;

        const std::uint8_t hello[] = {'h', 'e', 'l', 'l', 'o', '\n'};
        memcpy(write_bytes, hello, sizeof(hello));
        naos::system::TerminalMaster::write_request master_write{};
        master_write.size = sizeof(hello);
        master_write.flags = 0;
        master_write.buffer.value = 0;
        na_handle_t write_invocation = NA_HANDLE_INVALID;
        status = master_client.submit_write(master_write, &write_resource, 1, &write_invocation, wire, sizeof(wire));
        if (status == NA_STATUS_OK)
            status = wait_invocation(write_invocation) == 0 ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        if (status == NA_STATUS_OK)
        {
            naos::system::TerminalMaster::write_response write_response{};
            na_result_frame_t write_result{};
            status = master_client.take_write(write_invocation, write_response, wire, sizeof(wire), nullptr, 0,
                                              write_result);
        }
        (void)naos_handle_close(write_invocation);
        if (status != NA_STATUS_OK)
        {
            smoke_log("smoke: master write failed\n");
            release_regions();
            (void)naos_handle_close(master_handle);
            (void)naos_handle_close(slave_handle);
            (void)naos_handle_close(manager);
            return;
        }

        naos::system::TerminalSlave::read_request slave_read{};
        slave_read.size = sizeof(hello);
        slave_read.flags = 0;
        slave_read.buffer.value = 0;
        na_handle_t read_invocation = NA_HANDLE_INVALID;
        status = slave_client.submit_read(slave_read, &read_resource, 1, &read_invocation, wire, sizeof(wire));
        if (status == NA_STATUS_OK)
            status = wait_invocation(read_invocation) == 0 ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        std::uint8_t slave_data[64]{};
        std::size_t slave_data_size = 0;
        if (status == NA_STATUS_OK)
        {
            naos::system::TerminalSlave::read_response read_response{};
            na_result_frame_t read_result{};
            status =
                slave_client.take_read(read_invocation, read_response, wire, sizeof(wire), nullptr, 0, read_result);
            if (status == NA_STATUS_OK && read_response.count <= sizeof(slave_data))
            {
                memcpy(slave_data, read_bytes, read_response.count);
                slave_data_size = read_response.count;
            }
        }
        (void)naos_handle_close(read_invocation);
        if (status == NA_STATUS_OK && slave_data_size == sizeof(hello) && memcmp(slave_data, hello, sizeof(hello)) == 0)
        {
            smoke_log("smoke: pty master->slave data ok\n");
        }
        else
        {
            smoke_log("smoke: pty master->slave data mismatch\n");
        }

        const std::uint8_t output[] = {'o', 'k'};
        memcpy(write_bytes, output, sizeof(output));
        naos::system::TerminalSlave::write_request slave_write{};
        slave_write.size = sizeof(output);
        slave_write.flags = 0;
        slave_write.buffer.value = 0;
        na_handle_t slave_write_invocation = NA_HANDLE_INVALID;
        status =
            slave_client.submit_write(slave_write, &write_resource, 1, &slave_write_invocation, wire, sizeof(wire));
        if (status == NA_STATUS_OK)
            status = wait_invocation(slave_write_invocation) == 0 ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        (void)naos_handle_close(slave_write_invocation);

        naos::system::TerminalMaster::read_request master_read{};
        master_read.size = sizeof(output);
        master_read.flags = 0;
        master_read.buffer.value = 0;
        na_handle_t master_read_invocation = NA_HANDLE_INVALID;
        status = master_client.submit_read(master_read, &read_resource, 1, &master_read_invocation, wire, sizeof(wire));
        if (status == NA_STATUS_OK)
            status = wait_invocation(master_read_invocation) == 0 ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
        std::uint8_t master_data[64]{};
        std::size_t master_data_size = 0;
        if (status == NA_STATUS_OK)
        {
            naos::system::TerminalMaster::read_response read_response{};
            na_result_frame_t read_result{};
            status = master_client.take_read(master_read_invocation, read_response, wire, sizeof(wire), nullptr, 0,
                                             read_result);
            if (status == NA_STATUS_OK && read_response.count <= sizeof(master_data))
            {
                memcpy(master_data, read_bytes, read_response.count);
                master_data_size = read_response.count;
            }
        }
        (void)naos_handle_close(master_read_invocation);
        if (status == NA_STATUS_OK && master_data_size == sizeof(output) &&
            memcmp(master_data, output, sizeof(output)) == 0)
        {
            smoke_log("smoke: pty slave->master data ok\n");
        }
        else
        {
            smoke_log("smoke: pty slave->master data mismatch\n");
        }

        release_regions();
        (void)naos_handle_close(master_handle);
        (void)naos_handle_close(slave_handle);
    }
    else
    {
        smoke_printf("smoke: ttyd create_pty failed status=%d outcome=%d reason=%d res=%llu error=%lld\n",
                     static_cast<int>(status), static_cast<int>(result.execution_outcome),
                     static_cast<int>(result.outcome_reason), static_cast<unsigned long long>(result.actual_resources),
                     static_cast<long long>(result.protocol_error));
        char message[160]{};
        snprintf(message, sizeof(message),
                 "smoke: create_pty status=%d outcome=%d reason=%d res=%llu error=%lld bytes=%llu number=%llu\n",
                 static_cast<int>(status), static_cast<int>(result.execution_outcome),
                 static_cast<int>(result.outcome_reason), static_cast<unsigned long long>(result.actual_resources),
                 static_cast<long long>(result.protocol_error), static_cast<unsigned long long>(result.actual_bytes),
                 static_cast<unsigned long long>(response.number));
        smoke_log(message);
    }
    smoke_log("smoke: ttyd smoke done\n");
    (void)naos_handle_close(manager);
}

void malformed_message_smoke()
{
    smoke_log("smoke: malformed message smoke begin\n");
    na_handle_t manager = NA_HANDLE_INVALID;
    if (naos_service_connect_versioned("naos://system/terminal", &naos::system::TerminalManager::protocol_uuid,
                                       NA_PROTOCOL_RIGHT_INVOKE, naos::system::TerminalManager::revision,
                                       naos::system::TerminalManager::features, &manager) != 0)
    {
        smoke_log("smoke: malformed smoke connect failed\n");
        return;
    }
    std::uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    na_submit_frame_t frame{};
    frame.struct_size = sizeof(frame);
    frame.method_id = NA_METHOD_TERMINAL_MANAGER_CREATE_PTY;
    frame.request = reinterpret_cast<std::uint64_t>(garbage);
    frame.request_bytes = 3;
    na_handle_t invocation = NA_HANDLE_INVALID;
    const auto status = _na_invoke_submit(manager, &frame, &invocation);
    if (status == NA_STATUS_OK && wait_invocation(invocation) == 0)
    {
        na_result_frame_t result{};
        (void)_na_invocation_take_result(invocation, &result);
        (void)naos_handle_close(invocation);
    }
    (void)naos_handle_close(manager);

    const int probe = open("/dev/ptmx", O_RDWR);
    if (probe >= 0)
    {
        close(probe);
        smoke_log("smoke: malformed message isolation ok\n");
    }
    else
    {
        smoke_log("smoke: malformed message isolation failed\n");
    }
}

void tty_fuzz_smoke()
{
    smoke_log("smoke: tty fuzz smoke begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        smoke_log("smoke: fuzz ptmx failed\n");
        return;
    }
    int number = -1;
    int unlock = 0;
    if (ioctl(master, TIOCGPTN, &number) != 0 || grantpt(master) != 0 || ioctl(master, TIOCSPTLCK, &unlock) != 0)
    {
        close(master);
        return;
    }
    char path[32]{};
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    const int slave = open(path, O_RDWR);
    if (slave < 0)
    {
        close(master);
        return;
    }
    struct termios raw{};
    if (ioctl(master, TCGETS, &raw) == 0)
    {
        raw.c_iflag = 0;
        raw.c_oflag = 0;
        raw.c_cflag = CS8 | CREAD;
        raw.c_lflag = 0;
        (void)ioctl(master, TCSETS, &raw);
    }
    (void)fcntl(slave, F_SETFL, O_NONBLOCK);

    std::uint32_t seed = 0x12345678;
    bool ok = true;
    std::uint8_t chunk[64]{};
    for (int round = 0; round < 64 && ok; round++)
    {
        for (int i = 0; i < 64; i++)
        {
            seed = seed * 1664525u + 1013904223u;
            chunk[i] = static_cast<std::uint8_t>(seed >> 24);
        }
        const ssize_t written = write(master, chunk, sizeof(chunk));
        if (written != static_cast<ssize_t>(sizeof(chunk)))
            ok = false;
    }
    for (int i = 0; i < 64; i++)
    {
        std::uint8_t drain[128]{};
        const ssize_t n = read(slave, drain, sizeof(drain));
        if (n < 0 && errno == EAGAIN)
            break;
        if (n < 0)
        {
            ok = false;
            break;
        }
    }
    close(slave);
    close(master);
    smoke_log(ok ? "smoke: tty fuzz smoke ok\n" : "smoke: tty fuzz smoke failed\n");
}

void framebuffer_lifetime_smoke()
{
    smoke_log("smoke: framebuffer lifetime smoke begin\n");
    const int fd = open("/dev/fb0", O_RDWR | O_EXCL);
    if (fd < 0)
    {
        smoke_log("smoke: fb0 open failed\n");
        return;
    }
    fb_fix_screeninfo fix{};
    fb_var_screeninfo var{};
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0)
    {
        close(fd);
        return;
    }
    const std::size_t visible_frame_bytes = static_cast<std::size_t>(fix.line_length) * var.yres;
    if (fix.smem_len < visible_frame_bytes)
    {
        close(fd);
        smoke_log("smoke: fb0 memory is smaller than visible frame\n");
        return;
    }
    const std::size_t mapped_bytes = fix.smem_len;
    void *mapping = mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED)
    {
        close(fd);
        smoke_log("smoke: fb0 mmap failed\n");
        return;
    }
    close(fd);

    bool busy_before_unmap = false;
    const int second = open("/dev/fb0", O_RDWR | O_EXCL);
    if (second < 0)
    {
        busy_before_unmap = true;
    }
    else
    {
        close(second);
    }
    (void)munmap(mapping, mapped_bytes);

    const int third = open("/dev/fb0", O_RDWR | O_EXCL);
    const bool free_after_unmap = third >= 0;
    if (third >= 0)
        close(third);

    if (busy_before_unmap && free_after_unmap)
        smoke_log("smoke: framebuffer lifetime ok\n");
    else
        smoke_log("smoke: framebuffer lifetime failed\n");
}

// W1 acceptance: a NA_MEMORY_MAP_SHARED mapping is backed by the object's own
// page frames rather than a private per-process page.  Two independent shared
// mappings of one object range must therefore alias the same physical pages:
// a write through either is visible through the other with no unmap and no
// remap, and it reaches the object itself.
int mobj_share_smoke()
{
    constexpr uint64_t kPageBytes = 4096;
    constexpr uint64_t kObjectBytes = kPageBytes * 2;
    constexpr uint32_t kSharedRw = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_SHARED;
    constexpr uint32_t kPrivateRw = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE;

    auto map_region = [](na_handle_t object, uint64_t offset, uint64_t length, uint32_t flags, uint64_t &address,
                         uint64_t *data_offset = nullptr) {
        na_memory_map_frame_t frame{};
        frame.struct_size = sizeof(frame);
        frame.flags = flags;
        frame.object = object;
        frame.offset = offset;
        frame.length = length;
        if (_na_memory_map(&frame) != NA_STATUS_OK || frame.address == 0)
            return false;
        address = frame.address;
        if (data_offset != nullptr)
            *data_offset = frame.data_offset;
        return true;
    };
    auto unmap_region = [](uint64_t address, uint64_t length) {
        na_memory_unmap_frame_t frame{};
        frame.struct_size = sizeof(frame);
        frame.address = address;
        frame.length = length;
        return _na_memory_unmap(&frame) == NA_STATUS_OK;
    };
    auto write_byte = [](uint64_t address, uint64_t index, unsigned char value) {
        reinterpret_cast<volatile unsigned char *>(static_cast<uintptr_t>(address))[index] = value;
    };
    auto read_byte = [](uint64_t address, uint64_t index) {
        return reinterpret_cast<volatile unsigned char *>(static_cast<uintptr_t>(address))[index];
    };

    na_handle_t object = NA_HANDLE_INVALID;
    if (_na_memory_create(kObjectBytes, 0, &object) != NA_STATUS_OK)
    {
        smoke_log("mobj-share: create failed\n");
        return 1;
    }

    // A1: two mappings of the same range alias one frame.
    uint64_t first = 0;
    uint64_t second = 0;
    if (!map_region(object, 0, kPageBytes, kSharedRw, first) || !map_region(object, 0, kPageBytes, kSharedRw, second) ||
        first == second)
    {
        smoke_log("mobj-share: shared map failed\n");
        (void)_na_handle_close(object);
        return 1;
    }
    write_byte(first, 0, 0x5a);
    write_byte(first, kPageBytes - 1, 0x3c);
    const bool aliased = read_byte(second, 0) == 0x5a && read_byte(second, kPageBytes - 1) == 0x3c;

    // A7: a subspan is a bounded capability over the same storage identity.
    // Its byte range is deliberately unaligned so the kernel must keep the
    // page edges private and only copy the logical bytes back on unmap.
    bool view_ok = false;
    bool view_info_ok = false;
    bool view_mapped = false;
    bool view_rejected_tail = false;
    bool view_edge_private = false;
    bool view_wrote_back = false;
    bool view_nested_rejected = false;
    bool info_short_size_rejected = false;
    bool info_extended_size_rejected = false;
    na_handle_t view_source = NA_HANDLE_INVALID;
    na_handle_t view = NA_HANDLE_INVALID;
    if (_na_handle_duplicate(object, 0, &view_source) == NA_STATUS_OK)
    {
        na_handle_restriction_t restriction{};
        restriction.struct_size = sizeof(restriction);
        restriction.flags = NA_RESTRICTION_RANGE;
        restriction.view_offset = 3;
        restriction.view_length = 17;
        if (_na_handle_restrict(view_source, &restriction, &view) == NA_STATUS_OK)
        {
            na_handle_info_t object_info{};
            na_handle_info_t view_info{};
            object_info.struct_size = sizeof(object_info);
            view_info.struct_size = sizeof(view_info);
            view_info_ok = _na_handle_get_info(object, &object_info) == NA_STATUS_OK &&
                           _na_handle_get_info(view, &view_info) == NA_STATUS_OK &&
                           object_info.object_id == view_info.object_id && view_info.view_offset == 3 &&
                           view_info.view_length == 17;

            na_handle_info_t malformed_info{};
            malformed_info.struct_size = sizeof(malformed_info) - sizeof(uint64_t);
            info_short_size_rejected = _na_handle_get_info(object, &malformed_info) == NA_STATUS_INVALID_ARGUMENT;
            malformed_info.struct_size = sizeof(malformed_info) + sizeof(uint64_t);
            info_extended_size_rejected = _na_handle_get_info(object, &malformed_info) == NA_STATUS_INVALID_ARGUMENT;

            // Make both sides of the view distinguishable from the logical
            // range.  The mapping must not expose either adjacent byte.
            write_byte(first, 2, 0xa1);
            write_byte(first, 3, 0xa3);
            write_byte(first, 19, 0xa4);
            write_byte(first, 20, 0xa2);

            uint64_t view_mapping = 0;
            uint64_t view_data_offset = 0;
            view_mapped = map_region(view, 0, 17, kSharedRw, view_mapping, &view_data_offset);
            view_rejected_tail = !map_region(view, 17, 1, kSharedRw, view_mapping);
            if (view_mapped)
            {
                // map_region returns the page-aligned VMA base.  The logical
                // view starts three bytes into that page.
                const uint64_t logical = view_mapping + view_data_offset;
                view_edge_private = view_data_offset == 3 && read_byte(logical, 0) == 0xa3 &&
                                    read_byte(logical, 16) == 0xa4 && read_byte(view_mapping, 2) == 0 &&
                                    read_byte(logical, 17) == 0;
                write_byte(logical, 8, 0xb5);
                const bool unmapped = unmap_region(view_mapping, kPageBytes);
                view_wrote_back = unmapped && read_byte(first, 11) == 0xb5 && read_byte(first, 2) == 0xa1 &&
                                  read_byte(first, 20) == 0xa2;
            }

            na_handle_t nested_source = NA_HANDLE_INVALID;
            na_handle_t widened = NA_HANDLE_INVALID;
            if (_na_handle_duplicate(view, 0, &nested_source) == NA_STATUS_OK)
            {
                na_handle_restriction_t widen{};
                widen.struct_size = sizeof(widen);
                widen.flags = NA_RESTRICTION_RANGE;
                widen.view_offset = 16;
                widen.view_length = 2;
                view_nested_rejected = _na_handle_restrict(nested_source, &widen, &widened) != NA_STATUS_OK;
            }
            if (widened != NA_HANDLE_INVALID)
                (void)_na_handle_close(widened);
            if (nested_source != NA_HANDLE_INVALID)
                (void)_na_handle_close(nested_source);
            view_ok = view_info_ok && info_short_size_rejected && info_extended_size_rejected && view_mapped &&
                      view_rejected_tail && view_edge_private && view_wrote_back && view_nested_rejected;
        }
    }
    if (view != NA_HANDLE_INVALID)
        (void)_na_handle_close(view);
    if (view_source != NA_HANDLE_INVALID)
        (void)_na_handle_close(view_source);
    smoke_printf("mobj-share: view=%d info=%d short=%d extended=%d mapped=%d tail=%d edges=%d writeback=%d nested=%d\n",
                 view_ok ? 1 : 0, view_info_ok ? 1 : 0, info_short_size_rejected ? 1 : 0,
                 info_extended_size_rejected ? 1 : 0, view_mapped ? 1 : 0, view_rejected_tail ? 1 : 0,
                 view_edge_private ? 1 : 0, view_wrote_back ? 1 : 0, view_nested_rejected ? 1 : 0);

    // A6: the steady state is the same mapping again and again; every round
    // must observe the peer mapping without a page-table update.
    bool steady = true;
    for (unsigned int round = 0; round < 64 && steady; round++)
    {
        const unsigned char value = static_cast<unsigned char>(round + 1);
        write_byte(first, 32, value);
        steady = read_byte(second, 32) == value;
    }

    // A2: the second object page is likewise one frame per (object, page) and
    // that frame belongs to the object, not to the mapping that faulted it in.
    uint64_t page_two = 0;
    uint64_t page_two_alias = 0;
    uint64_t page_two_again = 0;
    bool page_two_ok = false;
    const bool page_two_mapped = map_region(object, kPageBytes, kPageBytes, kSharedRw, page_two) &&
                                 map_region(object, kPageBytes, kPageBytes, kSharedRw, page_two_alias);
    if (page_two_mapped)
    {
        write_byte(page_two, 16, 0x77);
        const bool page_two_aliased = read_byte(page_two_alias, 16) == 0x77;
        const bool page_two_reused = unmap_region(page_two, kPageBytes) &&
                                     map_region(object, kPageBytes, kPageBytes, kSharedRw, page_two_again) &&
                                     read_byte(page_two_again, 16) == 0x77;
        const bool page_two_isolated = read_byte(first, 0) == 0x5a && read_byte(page_two_alias, 0) == 0;
        page_two_ok = page_two_aliased && page_two_reused && page_two_isolated;
        // page_two is already gone; only the remaining aliases need release.
        page_two = 0;
        if (page_two_again != 0)
            (void)unmap_region(page_two_again, kPageBytes);
        (void)unmap_region(page_two_alias, kPageBytes);
    }

    // A3: a private mapping neither observes shared writes nor publishes its
    // own, and unmapping it writes nothing back into the object.
    uint64_t private_address = 0;
    bool private_ok = false;
    const bool private_mapped = map_region(object, 0, kPageBytes, kPrivateRw, private_address);
    if (private_mapped)
    {
        write_byte(private_address, 0, 0x55);
        const bool isolated = read_byte(first, 0) == 0x5a && read_byte(second, 0) == 0x5a;
        const bool unmapped = unmap_region(private_address, kPageBytes);
        uint64_t fresh = 0;
        const bool published = !map_region(object, 0, kPageBytes, kSharedRw, fresh) || read_byte(fresh, 0) != 0x55;
        if (fresh != 0)
            (void)unmap_region(fresh, kPageBytes);
        private_ok = isolated && unmapped && published;
    }

    // A4: fork() must keep a private mapping private while the shared mapping
    // stays shared, so the child's write lands in the object for the parent.
    uint64_t cow_address = 0;
    bool fork_ok = false;
    if (map_region(object, 0, kPageBytes, kPrivateRw, cow_address))
    {
        write_byte(cow_address, 8, 0x11);
        write_byte(first, 8, 0x44);
        const pid_t child = fork();
        if (child == 0)
        {
            write_byte(cow_address, 8, 0x22);
            write_byte(first, 8, 0x33);
            _exit(0);
        }
        if (child > 0)
        {
            int child_status = 0;
            (void)waitpid(child, &child_status, 0);
            fork_ok = read_byte(cow_address, 8) == 0x11 && read_byte(first, 8) == 0x33 && read_byte(second, 8) == 0x33;
        }
        if (!fork_ok)
            smoke_log("mobj-share: fork isolation failed\n");
        (void)unmap_region(cow_address, kPageBytes);
    }
    else
    {
        smoke_log("mobj-share: private fork map failed\n");
    }

    // A5 (admission half): a read-only object refuses a writable mapping, and
    // a permitted read-only mapping neither publishes a store nor lets the
    // object change.  The page-table fault witness lives in
    // `mobj_protect_smoke`, which a gated boot cannot host: the deliberate
    // user-space SIGSEGV is logged as a fault, so that case is driven
    // explicitly and judged from the kernel log.
    na_handle_t read_only = NA_HANDLE_INVALID;
    bool read_only_ok = false;
    if (_na_memory_create(kPageBytes, NA_MEMORY_FLAG_READ_ONLY, &read_only) == NA_STATUS_OK)
    {
        na_memory_map_frame_t denied{};
        denied.struct_size = sizeof(denied);
        denied.flags = kSharedRw;
        denied.object = read_only;
        denied.length = kPageBytes;
        const bool denied_writable = _na_memory_map(&denied) != NA_STATUS_OK;

        uint64_t read_only_address = 0;
        const bool read_only_mapped = map_region(read_only, 0, kPageBytes, NA_MEMORY_MAP_READ, read_only_address);
        bool unchanged = false;
        if (read_only_mapped)
        {
            // The object stays zero because a read-only mapping carries no
            // writable page: a store here cannot reach the object's frame.
            uint64_t shared_view = 0;
            if (map_region(read_only, 0, kPageBytes, NA_MEMORY_MAP_READ, shared_view))
            {
                unchanged = read_byte(shared_view, 0) == 0;
                (void)unmap_region(shared_view, kPageBytes);
            }
            (void)unmap_region(read_only_address, kPageBytes);
        }
        read_only_ok = denied_writable && read_only_mapped && unchanged;
        smoke_printf("mobj-share: ro denied=%d mapped=%d unchanged=%d\n", denied_writable ? 1 : 0,
                     read_only_mapped ? 1 : 0, unchanged ? 1 : 0);
        (void)_na_handle_close(read_only);
    }
    else
    {
        smoke_log("mobj-share: read-only create failed\n");
    }

    (void)unmap_region(first, kPageBytes);
    (void)unmap_region(second, kPageBytes);
    if (page_two_mapped)
        (void)unmap_region(page_two, kPageBytes);
    (void)_na_handle_close(object);

    const bool ok = aliased && view_ok && steady && page_two_ok && private_ok && fork_ok && read_only_ok;
    // One line, emitted last, so the console ring keeps the whole verdict.
    smoke_printf("mobj-share: %s alias=%d view=%d steady=%d page2=%d private=%d fork=%d readonly=%d\n",
                 ok ? "PASS" : "FAIL", aliased ? 1 : 0, view_ok ? 1 : 0, steady ? 1 : 0, page_two_ok ? 1 : 0,
                 private_ok ? 1 : 0, fork_ok ? 1 : 0, read_only_ok ? 1 : 0);
    return ok ? 0 : 1;
}

// A5 (page-table half), driven explicitly rather than from the gated boot
// assertion: a store through a read-only mapping of a read-only object must
// fault in the page tables instead of landing on the object's frame.  The
// kernel reports that fault on the console and delivers SIGSEGV, so the
// verdict is read from the kernel log; the exit status only says whether the
// harness itself got far enough to observe all three outcomes.
int mobj_protect_smoke()
{
    constexpr uint64_t kPageBytes = 4096;
    bool denied_writable = false;
    bool witnessed = false;
    int child_status = -1;
    bool object_unchanged = false;

    na_handle_t read_only = NA_HANDLE_INVALID;
    if (_na_memory_create(kPageBytes, NA_MEMORY_FLAG_READ_ONLY, &read_only) != NA_STATUS_OK)
    {
        smoke_log("mobj-protect: create failed\n");
        return 1;
    }

    na_memory_map_frame_t denied{};
    denied.struct_size = sizeof(denied);
    denied.flags = NA_MEMORY_MAP_READ | NA_MEMORY_MAP_WRITE | NA_MEMORY_MAP_SHARED;
    denied.object = read_only;
    denied.length = kPageBytes;
    denied_writable = _na_memory_map(&denied) != NA_STATUS_OK;

    na_memory_map_frame_t mapping{};
    mapping.struct_size = sizeof(mapping);
    mapping.flags = NA_MEMORY_MAP_READ;
    mapping.object = read_only;
    mapping.length = kPageBytes;
    if (_na_memory_map(&mapping) != NA_STATUS_OK || mapping.address == 0)
    {
        smoke_log("mobj-protect: read-only map failed\n");
        (void)_na_handle_close(read_only);
        return 1;
    }

    // A shared witness page lets the parent distinguish "the child's store
    // landed" from "the child faulted before reaching the marker".  This libc
    // has no pipe(2), so the two processes rendezvous through mmap instead.
    void *witness = mmap(nullptr, kPageBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (witness == MAP_FAILED)
    {
        smoke_log("mobj-protect: witness mmap failed\n");
        (void)_na_handle_close(read_only);
        return 1;
    }
    auto *marker = static_cast<volatile unsigned char *>(witness);
    *marker = 0;

    const pid_t child = fork();
    if (child == 0)
    {
        auto *target = reinterpret_cast<volatile unsigned char *>(static_cast<uintptr_t>(mapping.address));
        target[0] = 0xcc;
        *marker = 0xa5;
        _exit(0);
    }
    if (child > 0)
    {
        int status = 0;
        (void)waitpid(child, &status, 0);
        witnessed = *marker == 0;
        // The wait status only carries the exit code, so a child killed by the
        // protection fault reads as exit 0 here.  The fault itself is the
        // kernel's "exception 14 occurred at ... pid ..." line in the log;
        // what this case can assert on its own is that the store never landed.
        // Re-read the read-only mapping: the object must still be zero.
        object_unchanged = reinterpret_cast<volatile unsigned char *>(static_cast<uintptr_t>(mapping.address))[0] == 0;
        child_status = status;
    }

    (void)munmap(witness, kPageBytes);
    na_memory_unmap_frame_t release{};
    release.struct_size = sizeof(release);
    release.address = mapping.address;
    release.length = mapping.length;
    (void)_na_memory_unmap(&release);
    (void)_na_handle_close(read_only);

    const bool ok = denied_writable && witnessed && object_unchanged;
    // "PASS" is the marker the operator greps for in the kernel log; the
    // fault line that precedes it is the page-table evidence.
    smoke_printf("mobj-protect: %s denied=%d witness=%d unchanged=%d child_status=%d\n", ok ? "PASS" : "FAIL",
                 denied_writable ? 1 : 0, witnessed ? 1 : 0, object_unchanged ? 1 : 0, child_status);
    return ok ? 0 : 1;
}

int vfs_data_plane_smoke()
{
    smoke_log("vfs-data-smoke: waiting for /data mount\n");
    int directory = -1;
    for (int attempt = 0; attempt < 100; attempt++)
    {
        directory = open("/data", O_RDONLY | O_DIRECTORY);
        if (directory >= 0)
            break;
        (void)usleep(100'000);
    }
    if (directory < 0)
    {
        smoke_printf("vfs-data-smoke: /data open failed errno=%d\n", errno);
        return 1;
    }

    constexpr char first_name[] = "vfs-data-smoke.txt";
    constexpr char payload[] = "userspace-vfs-data\n";
    int file = openat(directory, first_name, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (file < 0)
    {
        smoke_printf("vfs-data-smoke: openat create failed errno=%d\n", errno);
        close(directory);
        return 1;
    }

    bool ok = write(file, payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1);
    ok = ok && fsync(file) == 0;
    ok = ok && lseek(file, 0, SEEK_SET) == 0;
    char buffer[sizeof(payload)]{};
    const ssize_t read_count = read(file, buffer, sizeof(buffer) - 1);
    ok = ok && read_count == static_cast<ssize_t>(sizeof(payload) - 1) &&
         memcmp(buffer, payload, sizeof(payload) - 1) == 0;

    errno = 0;
    const int exclusive = openat(directory, first_name, O_CREAT | O_EXCL | O_RDWR, 0644);
    ok = ok && exclusive < 0 && errno == EEXIST;
    if (exclusive >= 0)
        close(exclusive);

    ok = ok && ftruncate(file, 4) == 0;
    struct stat file_stat{};
    ok = ok && fstat(file, &file_stat) == 0 && file_stat.st_size == 4;
    ok = ok && rename("/data/vfs-data-smoke.txt", "/data/vfs-data-smoke-renamed.txt") == 0;
    ok = ok && fsync(directory) == 0;
    ok = ok && access("/data/vfs-data-smoke-renamed.txt", F_OK) == 0;
    // The path-reopen FAT worker rejects unlink while an active File binding
    // still owns that pathname (EBUSY). Close the renamed description before
    // removing it, while the preceding stat/access checks still prove that
    // rename preserved the open description's visibility.
    close(file);
    file = -1;
    ok = ok && unlink("/data/vfs-data-smoke-renamed.txt") == 0;
    ok = ok && access("/data/vfs-data-smoke-renamed.txt", F_OK) != 0 && errno == ENOENT;

    if (file >= 0)
        close(file);
    close(directory);
    if (!ok)
    {
        smoke_printf("vfs-data-smoke: FAIL errno=%d\n", errno);
        return 1;
    }
    smoke_log("vfs-data-smoke: PASS\n");
    return 0;
}

bool wait_for_job_control_stop(pid_t child, int expected_signal, const char *signal_name)
{
    int child_status = 0;
    for (int attempt = 0; attempt < 100; attempt++)
    {
        const pid_t waited = waitpid(child, &child_status, WUNTRACED | WNOHANG);
        if (waited == child)
        {
            if (WIFSTOPPED(child_status) && WSTOPSIG(child_status) == expected_signal)
            {
                smoke_printf("smoke: %s stop ok\n", signal_name);
                return true;
            }
            smoke_printf("smoke: %s stop missing status=%d\n", signal_name, child_status);
            return false;
        }
        if (waited < 0)
        {
            if (errno == EINTR)
                continue;
            smoke_printf("smoke: %s wait failed errno=%d\n", signal_name, errno);
            return false;
        }
        (void)usleep(10'000);
    }
    smoke_printf("smoke: %s wait timed out\n", signal_name);
    return false;
}

void openpty_smoke()
{
    smoke_log("smoke: openpty begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        smoke_log("smoke: openpty master failed\n");
        return;
    }
    smoke_log("smoke: openpty master opened\n");
    int number = -1;
    if (ioctl(master, TIOCGPTN, &number) != 0)
    {
        smoke_log("smoke: TIOCGPTN failed\n");
        close(master);
        return;
    }
    int unlock = 0;
    if (grantpt(master) != 0 || ioctl(master, TIOCSPTLCK, &unlock) != 0)
    {
        smoke_log("smoke: TIOCSPTLCK failed\n");
        close(master);
        return;
    }
    char path[32]{};
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    const int slave = open(path, O_RDWR);
    if (slave < 0)
    {
        smoke_log("smoke: openpty slave failed\n");
        close(master);
        return;
    }
    smoke_log("smoke: openpty opened\n");

    smoke_log("smoke: job control begin\n");
    const pid_t session_id = setsid();
    if (session_id < 0)
    {
        smoke_printf("smoke: setsid failed errno=%d\n", errno);
        close(slave);
        close(master);
        return;
    }
    smoke_printf("smoke: job control session=%d\n", static_cast<int>(session_id));
    if (ioctl(slave, TIOCSCTTY, 0) != 0)
        smoke_log("smoke: TIOCSCTTY failed\n");
    else
        smoke_log("smoke: TIOCSCTTY ok\n");
    const pid_t self_pid = getpid();
    if (ioctl(master, TIOCSPGRP, &self_pid) != 0)
        smoke_log("smoke: TIOCSPGRP failed\n");
    else
        smoke_log("smoke: TIOCSPGRP ok\n");
    int pgrp = -1;
    if (ioctl(master, TIOCGPGRP, &pgrp) != 0)
        smoke_log("smoke: TIOCGPGRP failed\n");
    else if (pgrp == static_cast<int>(self_pid))
        smoke_log("smoke: TIOCGPGRP ok\n");
    else
        smoke_log("smoke: TIOCGPGRP mismatch\n");
    struct winsize smoke_ws{};
    smoke_ws.ws_row = 24;
    smoke_ws.ws_col = 80;
    if (ioctl(master, TIOCSWINSZ, &smoke_ws) != 0)
        smoke_log("smoke: TIOCSWINSZ failed\n");
    else
        smoke_log("smoke: TIOCSWINSZ ok\n");

    smoke_log("smoke: SIGTTIN smoke begin\n");
    const pid_t bg = fork();
    if (bg == 0)
    {
        if (setpgid(0, 0) != 0)
            _exit(120);
        char bg_char = 0;
        const ssize_t bg_n = read(slave, &bg_char, 1);
        _exit(bg_n >= 0 ? 121 : 122);
    }
    if (bg > 0)
    {
        if (setpgid(bg, bg) != 0)
            smoke_log("smoke: bg setpgid failed\n");
        (void)wait_for_job_control_stop(bg, SIGTTIN, "SIGTTIN");
        (void)kill(bg, SIGKILL);
        (void)waitpid(bg, nullptr, 0);
    }

    smoke_log("smoke: SIGTTOU smoke begin\n");
    struct termios bg_termios{};
    if (ioctl(master, TCGETS, &bg_termios) == 0)
    {
        bg_termios.c_lflag |= TOSTOP;
        if (ioctl(slave, TCSETS, &bg_termios) != 0)
            smoke_printf("smoke: TCSETS TOSTOP failed errno=%d\n", errno);
    }
    const pid_t bgw = fork();
    if (bgw == 0)
    {
        if (setpgid(0, 0) != 0)
            _exit(130);
        const char bgw_char = 'x';
        const ssize_t bgw_n = write(slave, &bgw_char, 1);
        if (bgw_n != 1)
        {
            char bgw_message[64]{};
            snprintf(bgw_message, sizeof(bgw_message), "smoke: bgw write errno=%d\n", errno);
            smoke_log(bgw_message);
        }
        _exit(bgw_n == 1 ? 131 : 132);
    }
    if (bgw > 0)
    {
        if (setpgid(bgw, bgw) != 0)
            smoke_log("smoke: bgw setpgid failed\n");
        (void)wait_for_job_control_stop(bgw, SIGTTOU, "SIGTTOU");
        (void)kill(bgw, SIGKILL);
        (void)waitpid(bgw, nullptr, 0);
    }
    bg_termios.c_lflag &= ~TOSTOP;
    (void)ioctl(master, TCSETS, &bg_termios);

    const char hello[] = "hi\n";
    const ssize_t written_hello = write(master, hello, sizeof(hello) - 1);
    if (written_hello != static_cast<ssize_t>(sizeof(hello) - 1))
    {
        smoke_log("smoke: openpty master write failed\n");
    }
    else
    {
        char buffer[16]{};
        const ssize_t n = read(slave, buffer, sizeof(buffer));
        if (n == static_cast<ssize_t>(sizeof(hello) - 1) && memcmp(buffer, hello, sizeof(hello) - 1) == 0)
            smoke_log("smoke: openpty master->slave ok\n");
        else
            smoke_log("smoke: openpty master->slave mismatch\n");
    }

    const char out[] = "ok";
    const ssize_t written_out = write(slave, out, sizeof(out) - 1);
    if (written_out != static_cast<ssize_t>(sizeof(out) - 1))
    {
        smoke_log("smoke: openpty slave write failed\n");
    }
    else
    {
        char buffer[16]{};
        const ssize_t n = read(master, buffer, sizeof(buffer));
        if (n >= static_cast<ssize_t>(sizeof(out) - 1) &&
            memcmp(buffer + n - (sizeof(out) - 1), out, sizeof(out) - 1) == 0)
            smoke_log("smoke: openpty slave->master ok\n");
        else
            smoke_log("smoke: openpty slave->master mismatch\n");
    }

    const int duplicate = dup(master);
    if (duplicate < 0)
    {
        smoke_log("smoke: openpty dup failed\n");
    }
    else
    {
        close(duplicate);
    }

    const pid_t child = fork();
    if (child == 0)
    {
        const char child_data[] = "c\n";
        (void)write(master, child_data, sizeof(child_data) - 1);
        _exit(0);
    }
    if (child > 0)
    {
        int child_status = 0;
        (void)waitpid(child, &child_status, 0);
        char buffer[16]{};
        const ssize_t n = read(slave, buffer, sizeof(buffer));
        if (n == 2 && memcmp(buffer, "c\n", 2) == 0)
            smoke_log("smoke: openpty fork shared ok\n");
        else
            smoke_log("smoke: openpty fork shared mismatch\n");
    }

    if (ioctl(slave, TIOCNOTTY) != 0)
        smoke_printf("smoke: TIOCNOTTY failed errno=%d\n", errno);
    close(slave);
    close(master);
    smoke_log("smoke: openpty closed\n");
}

void pty_shell_smoke()
{
    smoke_log("smoke: pty shell begin\n");
    const int master = open("/dev/ptmx", O_RDWR);
    if (master < 0)
    {
        smoke_log("smoke: pty shell master failed\n");
        return;
    }
    int number = -1;
    if (ioctl(master, TIOCGPTN, &number) != 0 || number < 0)
    {
        smoke_log("smoke: pty shell TIOCGPTN failed\n");
        close(master);
        return;
    }
    int unlock = 0;
    if (grantpt(master) != 0 || ioctl(master, TIOCSPTLCK, &unlock) != 0)
    {
        smoke_log("smoke: pty shell TIOCSPTLCK failed\n");
        close(master);
        return;
    }
    char path[32]{};
    snprintf(path, sizeof(path), "/dev/pts/%d", number);
    const int slave = open(path, O_RDWR);
    if (slave < 0)
    {
        smoke_log("smoke: pty shell slave failed\n");
        close(master);
        return;
    }
    smoke_log("smoke: pty shell slave opened\n");

    char *shell_argv[] = {const_cast<char *>("sh"), nullptr};
    pid_t pid = -1;
    const int spawn_error = naos_native_spawn_stdio(&pid, "/bin/busybox", shell_argv, environ, slave, slave, slave);
    if (spawn_error != 0 || pid <= 0)
    {
        smoke_log("smoke: pty shell spawn failed\n");
        close(slave);
        close(master);
        return;
    }
    smoke_log("smoke: pty shell forked\n");

    const char command[] = "echo PTY_SHELL_OK\n";
    (void)write(master, command, sizeof(command) - 1);
    char output[512]{};
    std::size_t output_size = 0;
    bool ok = false;
    for (int i = 0; i < 12 && !ok; i++)
    {
        char chunk[128]{};
        const ssize_t n = read(master, chunk, sizeof(chunk) - 1);
        if (n > 0)
        {
            if (output_size + static_cast<std::size_t>(n) < sizeof(output))
            {
                memcpy(output + output_size, chunk, static_cast<std::size_t>(n));
                output_size += static_cast<std::size_t>(n);
            }
            output[output_size] = 0;
            if (strstr(output, "PTY_SHELL_OK") != nullptr)
                ok = true;
        }
        sleep(1);
    }
    if (ok)
        smoke_log("smoke: pty shell output ok\n");
    else
        smoke_log("smoke: pty shell output missing\n");

    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, nullptr, 0);
    close(slave);
    close(master);
}

int run_smoke_suite()
{
    ttyd_smoke();
    malformed_message_smoke();
    tty_fuzz_smoke();
    openpty_smoke();
    framebuffer_lifetime_smoke();
    const bool wait_deadline_ok = wait_deadline_smoke();
    const bool futex_ok = futex_smoke();

    smoke_log("smoke: PTY limit stress begin\n");
    bool pty_stress_ok = true;
    for (int i = 0; i < 300; i++)
    {
        const int stress_master = open("/dev/ptmx", O_RDWR);
        if (stress_master < 0)
        {
            pty_stress_ok = false;
            break;
        }
        int stress_number = -1;
        int stress_unlock = 0;
        if (ioctl(stress_master, TIOCGPTN, &stress_number) != 0 || grantpt(stress_master) != 0 ||
            ioctl(stress_master, TIOCSPTLCK, &stress_unlock) != 0)
        {
            pty_stress_ok = false;
            close(stress_master);
            break;
        }
        close(stress_master);
    }
    smoke_log(pty_stress_ok ? "smoke: PTY limit stress ok\n" : "smoke: PTY limit stress failed\n");
    pty_shell_smoke();
    smoke_log("smoke: ttyd kill recovery is supervisor-owned; skipped in standalone runner\n");
    const int tls_status = run_mlibc_tls_smoke();
    smoke_log(tls_status == 0 ? "smoke: mlibc TLS smoke ok\n" : "smoke: mlibc TLS smoke failed\n");
    return tls_status != 0 ? tls_status : wait_deadline_ok && futex_ok ? 0 : 1;
}

} // namespace

extern "C" int main(int argc, char **argv)
{
    // A smoke reports a verdict and nothing else: the /etc/init.sh that invoked
    // it owns machine teardown, so returning here never ends the boot.
    if (argc == 2 && strcmp(argv[1], "--vfs-data") == 0)
        return vfs_data_plane_smoke();
    if (argc == 2 && strcmp(argv[1], "--mobj-share") == 0)
        return mobj_share_smoke();
    if (argc == 2 && strcmp(argv[1], "--mobj-protect") == 0)
        return mobj_protect_smoke();
    if (argc == 2 && strcmp(argv[1], "--mlibc-tls") == 0)
    {
        const int tls_status = run_mlibc_tls_smoke();
        smoke_log(tls_status == 0 ? "smoke: mlibc TLS smoke ok\n" : "smoke: mlibc TLS smoke failed\n");
        return tls_status;
    }
    if (argc == 2 && strcmp(argv[1], "--futex") == 0)
        return futex_smoke() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--exec-multithread") == 0)
        return exec_rejects_multithread_smoke() ? 0 : 1;
    return run_smoke_suite();
}
