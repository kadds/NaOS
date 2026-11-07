#include "power_transition_command.hpp"

#include <cstdint>
#include <naos/generated/system/PlatformPowerControl.hpp>
#include <naos/libnao.hpp>
#include <naos/service_directory.hpp>
#include <stdio.h>
#include <string.h>

int run_power_transition_command(bool reboot, int argc, char **argv)
{
    const char *command = reboot ? "reboot" : "poweroff";
    bool verbose = false;
    for (int index = 1; index < argc; index++)
    {
        if (strcmp(argv[index], "-h") == 0 || strcmp(argv[index], "--help") == 0)
        {
            printf("usage: %s [-v]\n\n"
                   "Ask platformd to apply the typed system transition. If the machine remains running,\n"
                   "%s reports the kernel transition outcome and returns failure.\n",
                   command, command);
            return 0;
        }
        if (strcmp(argv[index], "-v") == 0)
        {
            verbose = true;
            continue;
        }
        printf("%s: unknown option '%s'\n", command, argv[index]);
        return 1;
    }

    na_handle_t raw_client = NA_HANDLE_INVALID;
    const int connect_error = naos_service_connect_versioned(
        NAOS_SERVICE_PLATFORM_POWER_CONTROL, &naos::system::PlatformPowerControl::protocol_uuid,
        NA_PROTOCOL_RIGHT_INVOKE, naos::system::PlatformPowerControl::revision,
        naos::system::PlatformPowerControl::features, &raw_client);
    if (connect_error != 0)
    {
        printf("%s: PlatformPowerControl unavailable (error %d)\n", command, connect_error);
        return 1;
    }
    nao::handle client(raw_client);

    na_submit_frame_t request{};
    request.struct_size = sizeof(request);
    request.method_id = reboot ? naos::system::PlatformPowerControl::method_reboot
                               : naos::system::PlatformPowerControl::method_power_off;
    printf("%s: submitting PlatformPowerControl %s request\n", command, reboot ? "reboot" : "power-off");
    fflush(stdout);
    na_handle_t raw_invocation = NA_HANDLE_INVALID;
    const auto submit_status = nao::event_loop::submit(client.get(), request, raw_invocation);
    if (submit_status != NA_STATUS_OK)
    {
        printf("%s: request submission failed (status %d)\n", command, static_cast<int>(submit_status));
        return 1;
    }
    nao::handle invocation(raw_invocation);
    const auto wait_status = nao::event_loop::wait_invocation(invocation.get());
    if (wait_status != NA_STATUS_OK)
    {
        printf("%s: request wait failed (status %d)\n", command, static_cast<int>(wait_status));
        return 1;
    }

    std::uint8_t response_wire[NA_CHANNEL_MAX_MESSAGE_BYTES]{};
    na_result_frame_t result{};
    result.struct_size = sizeof(result);
    result.bytes = reinterpret_cast<std::uint64_t>(response_wire);
    result.byte_capacity = sizeof(response_wire);
    const auto take_status = nao::event_loop::take(invocation.get(), result);
    if (take_status != NA_STATUS_OK)
    {
        printf("%s: response retrieval failed (status %d)\n", command, static_cast<int>(take_status));
        return 1;
    }
    if (result.execution_outcome != NA_EXECUTION_NONE || result.protocol_error != 0)
    {
        printf("%s: policy or kernel transition failed (execution=%u error=%lld)\n", command, result.execution_outcome,
               static_cast<long long>(result.protocol_error));
        return 1;
    }

    std::uint32_t outcome = 0;
    if (reboot)
    {
        naos::system::PlatformPowerControl::reboot_response response{};
        if (!naos::system::PlatformPowerControl::decode_reboot_response(response_wire, result.actual_bytes, response))
        {
            printf("%s: invalid PlatformPowerControl response\n", command);
            return 1;
        }
        outcome = response.result.outcome;
    }
    else
    {
        naos::system::PlatformPowerControl::power_off_response response{};
        if (!naos::system::PlatformPowerControl::decode_power_off_response(response_wire, result.actual_bytes,
                                                                           response))
        {
            printf("%s: invalid PlatformPowerControl response\n", command);
            return 1;
        }
        outcome = response.result.outcome;
    }

    printf("%s: platform remains running (transition outcome %u)\n", command, outcome);
    if (verbose)
        printf("%s: outcome 0=write committed, 1=failed, 2=hardware result unknown\n", command);
    return 1;
}
