#include "power_transition_command.hpp"

[[gnu::weak]] void *__dso_handle;

#ifdef __GNUC__
extern char __executable_start[];
[[gnu::weak]] void *__ehdr_start = __executable_start;
#endif

int main(int argc, char **argv) { return run_power_transition_command(true, argc, argv); }
