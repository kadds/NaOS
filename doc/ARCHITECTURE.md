# NaOS Architecture

## System Overview

NaOS is a freestanding 64-bit x86-64 operating system. GRUB loads the kernel using the Multiboot2 protocol; the same kernel and root filesystem image can be booted from an ISO or disk image. QEMU and Bochs are the primary development emulators. Kernel serial output is written to `run/kernel_out.log`.

## Source Organization

- `naos/src/kernel/` contains the kernel implementation: architecture setup, interrupts, devices, memory management, filesystems, I/O, scheduling, tasks, and system calls.
- `naos/includes/kernel/` contains public kernel headers, generally matching the subsystem layout under `src/kernel`.
- `naos/src/usr/` contains userland programs: `init` starts the user environment, `busybox/` is the pinned BusyBox source, and `bin/nanobox` remains as a legacy command binary.
- `naos/freelibcxx/` provides freestanding C++ containers and utilities used by the kernel.
- `naos/libc/mlibc/` provides the C library and startup objects used by userland programs.
- `naos/src/acpica/` supplies ACPICA integration for firmware and ACPI support.
- `run/` holds emulator configuration and the fake root filesystem; `util/` contains image, disk, symbol, and emulator helpers.

## Build and Image Flow

The build has two stages. Meson first cross-compiles static mlibc for the `naos` target. CMake then builds the kernel, `init`, and legacy `nanobox`, while an independent out-of-tree Make invocation builds pinned BusyBox 1.37.0 with `CONFIG_STATIC=y`, NaOS mlibc, and NaOS `crt1.o`. The stripped BusyBox binary is installed as `/bin/busybox`; the rootfs linker step creates `/bin/sh`, `/bin/ls`, `/bin/cat`, and the other configured applet links. CMake then generates kernel symbols, copies `run/fakeroot` into `build/bin/rfsroot`, and packages that directory as `build/bin/system/rfsimg`. The kernel is emitted as `build/bin/system/kernel`.

For an ISO boot, `python3 util/run.py q --iso` copies `kernel` and `rfsimg` into `run/iso` and invokes `grub-mkrescue`. For disk boot, the utility copies the system files into the mounted image’s `/boot` directory before starting the selected emulator.

## Extension Boundaries

Keep hardware- and CPU-specific code in `naos/src/kernel/arch`, reusable kernel facilities in their subsystem directories, and user-facing functionality under `naos/src/usr`. Kernel code must remain freestanding and avoid hosted-library, exception, and RTTI assumptions. Userland code should use mlibc rather than directly depending on host Linux APIs.
