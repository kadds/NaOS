//! `poweroff` (nanobox applet): power the machine off through ACPI S5.
//!
//! Machine teardown belongs to whoever drives the boot, not to a test binary:
//! an opt-in `/etc/init.sh` runs a smoke, reads its status, and then decides
//! that the boot is over.  Keeping the power-off in this applet lets that
//! script express it, so a smoke result can end the run immediately instead of
//! leaving the machine idle for the launcher's wall-clock limit.

#include <naos/syscall.h>
#include <stdio.h>
#include <string.h>

int poweroff(int argc, char **argv)
{
    bool verbose = false;
    for (int index = 1; index < argc; index++)
    {
        if (strcmp(argv[index], "-h") == 0 || strcmp(argv[index], "--help") == 0)
        {
            printf("usage: poweroff [-v]\n\n"
                   "Power the machine off. The ACPI S5 request does not return when the\n"
                   "firmware accepts it; a status is only reported when power management is\n"
                   "unavailable or the platform refuses the request.\n");
            return 0;
        }
        if (strcmp(argv[index], "-v") == 0)
        {
            verbose = true;
            continue;
        }
        printf("poweroff: unknown option '%s'\n", argv[index]);
        return 1;
    }

    const na_status_t status = _na_power_off();
    // Reaching this point means the request was not honoured.
    if (verbose)
        printf("poweroff: platform did not power off (status %d)\n", static_cast<int>(status));
    return status == NA_STATUS_OK ? 0 : 1;
}
