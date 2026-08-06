# NaOS Architecture

## System Overview

NaOS is a freestanding 64-bit x86-64 operating system. GRUB loads the kernel using the Multiboot2 protocol; the same kernel and root filesystem image can be booted from an ISO or disk image. QEMU and Bochs are the primary development emulators. Kernel serial output is written to `run/kernel_out.log`.

## Source Organization

- `naos/src/kernel/` contains the kernel implementation: architecture setup, interrupts, devices, memory management, filesystems, I/O, scheduling, tasks, and system calls.
- `naos/include/naos/` contains the user-visible native ABI and canonical wire headers. It is the only NaOS project include root
  exposed to userland targets.
- `naos/includes/kernel/` contains kernel-private headers, generally matching the subsystem layout under `src/kernel`; only the
  kernel target receives this include root.
- `naos/src/usr/` contains userland programs: `init` starts the user environment, `busybox/` is the pinned BusyBox source, and `bin/nanobox` remains as a legacy command binary.
- `naos/freelibcxx/` provides freestanding C++ containers and utilities used by the kernel.
- `naos/libc/mlibc/` provides the C library and startup objects used by userland programs.
- `naos/src/acpica/` supplies ACPICA integration for firmware and ACPI support.
- `run/` holds emulator configuration and the fake root filesystem; `util/` contains image, disk, symbol, and emulator helpers.

## Build and Image Flow

The build has two stages. Meson first cross-compiles static mlibc for the `naos` target. CMake then builds the kernel, `init`, and legacy `nanobox`, while an independent out-of-tree Make invocation builds pinned BusyBox 1.37.0 with `CONFIG_STATIC=y`, NaOS mlibc, and NaOS `crt1.o`. The stripped BusyBox binary is installed as `/bin/busybox`; the rootfs linker step creates `/bin/sh`, `/bin/ls`, `/bin/cat`, and the other configured applet links. CMake then generates kernel symbols, copies `run/fakeroot` into `build/bin/rfsroot`, and packages that directory as `build/bin/system/rfsimg`. The kernel is emitted as `build/bin/system/kernel`.

For an ISO boot, `python3 util/run.py q --iso -n` copies `kernel` and `rfsimg` into `run/iso` and invokes `grub-mkrescue`. For disk boot, the utility copies the system files into the mounted image’s `/boot` directory before starting the selected emulator. `-n` is the supported CI/development mode: QEMU remains headless and validation uses `run/kernel_out.log`.

## Extension Boundaries

Keep hardware- and CPU-specific code in `naos/src/kernel/arch`, reusable kernel facilities in their subsystem directories, and user-facing functionality under `naos/src/usr`. Kernel code must remain freestanding and avoid hosted-library, exception, and RTTI assumptions. Userland code should use mlibc rather than directly depending on host Linux APIs.

## Native Object/Capability Boundary

The native ABI is defined by [`OBJECT_CALL_PRD.md`](OBJECT_CALL_PRD.md) and
[`naos/abi.h`](../naos/include/naos/abi.h). A user-visible
handle is an opaque process-local `u64`; the kernel capability table stores the
binding, protocol scope, revision/features, meta rights, protocol rights, and
generation. RESERVED entries are never returned by lookup, and handle values
are monotonic for the lifetime of a process.

Raw channels own bounded FIFO messages. Resource dispositions are snapshotted
and committed transactionally, with MOVE/DUPLICATE attenuation and iterative
orphan-channel collection. Async protocol endpoints use immutable descriptors,
Invocation/Responder one-shot lifetimes, explicit cancellation/deadline
outcomes, and result take transactions. The system protocols are specified in
`naos/idl/system/`; `util/naoidl.py` generates their public UAPI, canonical wire
codecs, and typed bindings under each build directory's generated include root
(for example `build/naos/naos/generated/system`). The source tree contains only
`.naidl` schemas; ABI JSON manifests and generated headers never belong under
`naos/idl/system`. Both the mlibc compatibility edge and the kernel invocation dispatcher consume those
generated request/response codecs. The schemas therefore define the native
method ordinals and payload layouts shared by applications and the kernel.

Root/cwd are process path context, not entries in the capability table.
Bootstrap exposes native Directory, ServiceDirectory, and Stream capabilities; mlibc maps these
to its private POSIX fd table (where only the numeric indices 0/1/2,
`O_NONBLOCK`, `O_APPEND`, and `FD_CLOEXEC` exist). TTY and PTY control calls
are KernelView protocol methods, while terminal byte flow remains Stream data.
MemoryObject and shared-ring operations are typed KernelView object calls with
bounded limits and protocol-right checks; virtual-memory mapping remains a
separate address-space syscall.

ServiceDirectory is currently a kernel-backed typed registry carried through
bootstrap. `register` moves a resource capability into the registry,
`resolve` transfers it back to the caller, and `unregister` releases it;
non-unique objects may be resolved repeatedly. The userland service manager
and namespace policy remain the next layer above this primitive.

Native process creation is a two-stage transaction: the kernel creates a
deferred child with an empty resource table, transfers the executable and one
bootstrap-channel endpoint, and starts the child only after the endpoint is
installed. The parent then duplicates the explicitly selected namespace and
stdio capabilities into a fixed bootstrap message. The child consumes that
message once, closes the channel endpoint, and enters userland with only the
transferred capabilities; POSIX `posix_spawn` uses this path when no file
actions or spawn attributes require the compatibility implementation.

The old kernel `file_desc -> kobject` table, VFS `ioctl` switch, and native
open/read/write/dup2/fcntl/ioctl syscall registrations are not part of the
native boundary. The POSIX names in mlibc and applications are compatibility
surface only and dispatch through typed capabilities.

## Include Boundaries

The include dependency direction is intentionally one-way:

```text
userland / mlibc / generated protocol bindings
        │
        ├── naos/include/naos   (public ABI and canonical wire)
        ├── naos/idl/system     (only `.naidl` protocol source)
        └── build/naos/naos/generated (ABI JSON and public generated bindings)

kernel implementation
        ├── naos/include/naos   (public ABI consumed at syscall boundaries)
        └── naos/includes/kernel (private kernel mechanisms and concrete objects)
```

Kernel sources include private headers as `kernel/...`; userland and generated
bindings include public headers as `naos/...`. The build does not install the
kernel include root on userland targets, so a userland component cannot acquire
VFS, scheduler, object-reference, or other kernel-only definitions by include
path accident. NaoIDL source files are protocol specifications, not headers;
their generated public artifacts belong under the build-directory public include
root. CMake and Meson regenerate them before compiling their consumers; the
repository contains no generated system headers, and userland never receives
the kernel-private include root.
