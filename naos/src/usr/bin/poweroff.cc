#include "power_transition_command.hpp"

int poweroff(int argc, char **argv) { return run_power_transition_command(false, argc, argv); }
