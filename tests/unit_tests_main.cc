#include <cstdio>

int run_cfs_vtime_order_tests();
int run_phase1_channel_queue_tests();
int run_phase0_abi_tests();
int run_bootstrap_contract_tests();
int run_capability_restrict_policy_tests();
int run_system_idl_tests();
int run_invocation_deadline_tests();
int run_system_binding_contract_tests();
int run_service_directory_contract_tests();
int run_signal_policy_tests();
int run_wait_deadline_tests();
int run_ttyd_terminal_core_tests();
int run_syscall_header_tests();
int run_framebuffer_abi_tests();

namespace
{
struct test_suite
{
    const char *name;
    int (*run)();
};

constexpr test_suite suites[] = {
    {"cfs_vtime_order", run_cfs_vtime_order_tests},
    {"phase1_channel_queue", run_phase1_channel_queue_tests},
    {"phase0_abi", run_phase0_abi_tests},
    {"bootstrap_contract", run_bootstrap_contract_tests},
    {"capability_restrict_policy", run_capability_restrict_policy_tests},
    {"system_idl", run_system_idl_tests},
    {"invocation_deadline", run_invocation_deadline_tests},
    {"system_binding_contract", run_system_binding_contract_tests},
    {"service_directory_contract", run_service_directory_contract_tests},
    {"signal_policy", run_signal_policy_tests},
    {"wait_deadline", run_wait_deadline_tests},
    {"ttyd_terminal_core", run_ttyd_terminal_core_tests},
    {"syscall_header", run_syscall_header_tests},
    {"framebuffer_abi", run_framebuffer_abi_tests},
};
} // namespace

int main()
{
    for (const auto &suite : suites)
    {
        const int result = suite.run();
        if (result != 0)
        {
            std::fprintf(stderr, "unit suite %s failed (%d)\n", suite.name, result);
            return result;
        }
    }
    std::printf("%zu unit suites passed\n", sizeof(suites) / sizeof(suites[0]));
    return 0;
}
