#include "kernel/capability.hpp"

static_assert(static_cast<unsigned>(capability::entry_state::restricting) == 2);
static_assert(NA_PROCESS_RIGHT_JOB_CONTROL == (static_cast<na_meta_rights_t>(1) << 2));
static_assert(capability::derive_tty_control_rights(NA_RIGHT_WAIT) == NA_RIGHT_WAIT);
static_assert(capability::derive_tty_control_rights(NA_RIGHT_DUPLICATE | NA_RIGHT_WAIT) == NA_RIGHT_WAIT);
static_assert(capability::derive_tty_control_rights(NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT) ==
              (NA_RIGHT_TRANSFER | NA_RIGHT_INSPECT));

int main() { return capability::entry_state::restricting == capability::entry_state::active ? 1 : 0; }
