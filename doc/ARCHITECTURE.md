# NaOS Architecture

## System Overview

NaOS is a freestanding 64-bit x86-64 operating system. GRUB loads the kernel using the Multiboot2 protocol; the same kernel and root filesystem image can be booted from an ISO or disk image. QEMU and Bochs are the primary development emulators. Kernel serial output is written to `<build-dir>/kernel_out.log`.

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

The build has two stages. Meson first cross-compiles static mlibc for the `naos` target. CMake then builds the kernel, `init`, and legacy `nanobox`, while an independent out-of-tree Make invocation builds pinned BusyBox 1.37.0 with `CONFIG_STATIC=y`, NaOS mlibc, and NaOS `crt1.o`. The stripped BusyBox binary is installed as `/bin/busybox`; the rootfs linker step creates `/bin/sh`, `/bin/ls`, `/bin/cat`, and the other configured applet links. CMake generates `data/ksybs` as a userland/debug artifact, copies `run/fakeroot` into `<build-dir>/bin/rfsroot`, and builds the prepared FAT16 root image (8 KiB clusters) at `<build-dir>/bin/system/root.img`. The kernel is emitted as `<build-dir>/bin/system/kernel` and does not load `ksybs` at runtime.

For an ISO boot, `python3 util/run.py --build-dir build-debug q --iso -n` copies `kernel`, `root.img`, and the early-service modules into `build-debug/iso` and invokes `grub-mkrescue`, writing the ISO to `build-debug/image/naos.iso`. When `/dev/kvm` is available, the launcher enables KVM acceleration automatically; otherwise QEMU falls back to its default TCG accelerator. For disk boot, the utility writes the system files directly into `build-debug/image/disk.img:/boot` through `fstool`; no host mount or loop device is involved. `-n` is the supported CI/development mode: QEMU remains headless and agents inspect `build-debug/kernel_out.log` directly.

### Multiboot root and early-service modules

Every Multiboot module declares its authority token in the module command line.
The kernel copies those ranges into `kernel_start_args::named_modules`; `idle_task`
publishes the one-shot `rootfsd`, `init`, and `rootimage` resources, then starts
`vfsd` and `blockd`. `vfsd` resolves the prepared FAT16 image (8 KiB clusters) through the
published MemoryObject, while ramdiskd allocates its memory-backed disk and
publishes the userland block factory under the shared service namespace.
Filesystem workers are ordinary userland services: they resolve the factory and
VFS mount authority through ServiceDirectory. The early services are Rust
implementations, as assigned by the VFS/BlockDevice ADR r7.

The service namespace is hierarchical and shared by NaOS and Linux. The current
vertical slice publishes `naos://service/fs/vfs/0`,
`naos://service/fs/exfat/0`, and `naos://service/block/ramdiskd/0`.
`ServiceDirectory.list_prefix`/`ServiceLocator::list` enumerate all matching
instances below a URI prefix.

On Linux these names are concrete fixed-root UDS paths, so exfatd resolves
the block factory through the shared locator. On NaOS the factory is also a
ramdiskd-owned userland endpoint and the worker acquires a lease through that
same ServiceDirectory; no block lease or mount-control capability is embedded
in its bootstrap.

The named-module early-service handoff is now mandatory: `idle_task` starts
`vfsd` and `blockd`. After publishing its namespace listener, `vfsd` starts
the boot-module-provided `rootfsd` implementation (`exfatd`) with only the common
ServiceDirectory/stdio bootstrap; the worker must discover block and mount
authorities through services. `vfsd` then starts init with its mounted-root
and cwd endpoints. init owns only ttyd, consoled, the shell, and ordinary
service supervision; it does not spawn filesystem workers or receive block
capabilities. A boot without the `vfsd` module stops with an explicit
configuration error; it never opens `/bin/init` through the kernel root
filesystem.

## Kernel Logging and QEMU Debugging

The accepted logging architecture is recorded in
[`ADR_KERNEL_LOG.md`](ADR_KERNEL_LOG.md).

Kernel diagnostics use the fixed-module `KLOG_*` API in
[`kernel/log.hpp`](../naos/includes/kernel/log.hpp). Each call is retained as
one record with an internal sequence, level, timestamp, CPU, PID/TID, module,
source line, and bounded message. The sequence is not printed in the
human-facing text format. Structured messages escape control characters;
`KLOG_RAW` is reserved for stack, register, and firmware dumps that may span
multiple lines. The logger progresses through static early storage, the
runtime retention ring, and an emergency panic path. Normal serial, console,
and `/var/log/dmesg` output is handled by independent workers after the
scheduler starts. Userland `_s_log` records use the current process executable
basename, truncated to 12 characters, instead of the syscall implementation's
kernel module name; duplicate `name: ` prefixes in the message are removed.
Panic output includes the current register, MSR, control-register, and stack
dump before halting.

The relevant kernel command-line settings are `kernel_log_level`,
`kernel_log_sinker`, `kernel_log_buffer_size`,
`kernel_log_early_buffer_size`, `kernel_log_filter`,
`kernel_log_emergency_serial`, and `quiet`. Runtime storage defaults to 32 KiB
and accepts 8 KiB–1 MiB; early storage defaults to 8 KiB and accepts
1–8 KiB. The sinker format is
`<console|serial|dmesg>:<on|off>:<level>:<color|nocolor>`. Sink colors are
never stored in records or sent through the emergency serial path.

QEMU keeps the legacy GDB listener with `-s` on the default path. Use
`--wait-gdb` to add `-S`, `--gdb-port PORT` to select a validated TCP port,
`--qemu-debug` to write `<build-dir>/qemu.log`, `--monitor PATH` for a Unix monitor
socket, and `--no-reboot` to add `-no-reboot`. These options are
assembled identically for ISO, disk, and UEFI boots:

```bash
python3 util/run.py --build-dir build-debug q --iso -n --wait-gdb --gdb-port 12345 --no-reboot
python3 util/run.py --build-dir build-debug q --iso -n --qemu-debug --monitor /tmp/naos-qemu.monitor
```

An opt-in run (`--init-script`, used by the gated boot smokes) ends the boot from
inside the guest: the injected `/etc/init.sh` runs its smoke, records the status, and
then calls `/bin/poweroff`. Machine teardown belongs to whoever drives the boot, not to
a test binary, so a smoke only reports a verdict. `poweroff` is a `nanobox` applet over
`NA_SYSCALL_POWER_OFF`, which issues the ACPI S5 request the power button already uses.
This is why the guest-power-off path must stay enabled: a run that is finished would
otherwise idle until the launcher's `--timeout` limit.

## Extension Boundaries

Keep hardware- and CPU-specific code in `naos/src/kernel/arch`, reusable kernel facilities in their subsystem directories, and user-facing functionality under `naos/src/usr`. Kernel code must remain freestanding and avoid hosted-library, exception, and RTTI assumptions. Userland code should use mlibc rather than directly depending on host Linux APIs.

## Native Object/Capability Boundary

The native ABI is defined by [`OBJECT_CALL_ADR.md`](OBJECT_CALL_ADR.md) and
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
`idl/system/`; `idl/naoidl.py` generates their public UAPI, canonical wire
codecs, and typed bindings under each build directory's generated include root
(for example `build/naos/naos/generated/system`). The source tree contains only
`.naidl` schemas; ABI JSON manifests and generated headers never belong under
`idl/system`. Both the mlibc compatibility edge and the kernel invocation dispatcher consume those
generated request/response codecs. The schemas therefore define the native
method ordinals and payload layouts shared by applications and the kernel.

Root/cwd are process path context, not entries in the capability table.
Bootstrap exposes native Directory, ServiceDirectory, and Stream capabilities; mlibc maps these
to its private POSIX fd table (where only the numeric indices 0/1/2,
`O_NONBLOCK`, `O_APPEND`, and `FD_CLOEXEC` exist). TTY and PTY control calls
are protocol methods (`TerminalManager`/`TerminalMaster`/`TerminalSlave`),
with job control and driver signal delivery through the
`TerminalJobControl`/`TerminalDriverControl`/`TerminalDriverFactory`
KernelViews; terminal byte flow is no longer kernel Stream state. The line
discipline lives in the userland `ttyd` service, the renderer/keymap in
`consoled`, and kernel input is published as `InputEventSource` KeyEvents.
The kernel console pseudo devices only mirror early/emergency diagnostics.
MemoryObject operations are typed KernelView object calls with bounded limits
and protocol-right checks; virtual-memory mapping remains a separate
address-space syscall. User-space shared-memory rings live in `naos/libipc`
and do not create a kernel object.

`NA_SYSCALL_MEMORY_CREATE` (42) creates a bounded MemoryObject capability. The task loader uses the tagged `exec_source`
abstraction so the same ELF admission logic accepts either a VFS file or a MemoryObject; MemoryObject segments are mapped
through the VM's `memory_object` page-fault path. This is what permits boot modules and `File.materialize` products to be
executed without reintroducing a kernel pathname lookup.

A `NA_MEMORY_MAP_SHARED` mapping of a page-backed MemoryObject is served by the object's
own page frames rather than a private per-process page: the kernel view and every shared
mapping read and write the same bytes, so the steady state performs no per-request
payload copy and no page-table update. Private mappings keep the existing
fault-in-a-private-page and copy-back behavior, and `fork` keeps shared frames shared in
both address spaces while private frames stay COW. See
[`MEMORY_OBJECT_SHARING_ADR.md`](MEMORY_OBJECT_SHARING_ADR.md) for the admission,
lifetime and COW rules.

Userland bulk regions are the shared data plane on both platforms. `naos/libipc` owns the
transport-neutral channel/ring state machines; `servicekit` expresses one region request
for both transports through a single admission decision (`admit_service_region`) so the
same request is admitted or refused identically on NaOS and Linux, and each service
declares an admission bound that its transport enforces. See
[`DATAPLANE_MEASUREMENTS.md`](DATAPLANE_MEASUREMENTS.md) for the measured baseline and the
transport gating decision.

The service-side loop has one owner: `servicekit::Server::serve` runs the receive loop,
applies admission, and decides fault policy (a peer that disconnects or sends an
unacceptable frame costs that peer; anything else is fatal and becomes the exit code the
supervisor acts on). A service implements `ServeHandler` for everything after admission --
dispatch, ordering, and the reply -- which is where services legitimately differ. Dispatch
itself stays generated from NaoIDL, so servicekit does not depend on protocol bindings and
a service cannot drift from the schema's method set by hand.

mlibc now keeps root and cwd as runtime directory bindings and consumes Directory revision 2 methods
(`stat_node`, `sync`, `rename_at`, `link_at`, and `clone_binding`). It uses `File.materialize` as the bridge for private
file `MAP_PRIVATE` mappings. The following compatibility fallbacks are deliberately transitional and must be removed with
the corresponding service guarantees in Phase 4: the `renameat`/`linkat` unequal-dirfd `EXDEV` shortcut, directory-fd
`fsync`, open-plus-stat emulation, fork/spawn duplicate fallback, and the file-backed `MAP_PRIVATE` mapping fallback.

ServiceDirectory is currently a kernel-backed typed registry carried through
the common bootstrap contract. `register` moves a resource capability into the registry,
`resolve` transfers it back to the caller, and `unregister` releases it;
non-unique objects may be resolved repeatedly. Locator keys are canonical
local URIs such as `naos://system/console`; the protocol UUID identifies the
wire contract and is not a service locator. The userland service manager and
namespace policy remain the next layer above this primitive.

Native process creation is a two-stage transaction: the kernel creates a
deferred child with an empty resource table, transfers the executable and one
bootstrap-channel endpoint, and starts the child only after the endpoint is
installed. The parent then duplicates the explicitly selected namespace,
stdio into a versioned bootstrap message. The child consumes that message once,
closes the channel endpoint, and enters userland with only the transferred
startup resources; kernel-owned authorities are resolved by URI. POSIX `posix_spawn` uses this
path when no file actions or spawn attributes require the compatibility
implementation.

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
        ├── idl/system          (only `.naidl` protocol source, from root/idl submodule)
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
