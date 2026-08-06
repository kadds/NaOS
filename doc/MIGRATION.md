# Object/Capability Migration

This repository implements the Object Call PRD as a native capability ABI. The
migration is intentionally one-way: new kernel and userland code uses opaque
handles and typed protocols; the old native fd/object syscall ABI is not kept
as a compatibility target.

## Phase status

| Phase | Implemented boundary | Verification |
| --- | --- | --- |
| 0 | Opaque `u64` handles, generation-aware ACTIVE/RESERVED entries, two-layer rights, fault-safe usercopy, canonical ABI layout, and root/cwd outside the capability table | `phase0_abi_test`, kernel build and boot |
| 1 | Unique raw channel ends, bounded FIFO, atomic byte/resource snapshots, MOVE/DUPLICATE transfer, reserve/activate receive, wait signals, and hard limits | `phase1_channel_queue_test`, kernel build and boot |
| 2 | Immutable descriptors, typed endpoint roles, async Invocation, one-shot Responder, result ownership, cancellation, timer-driven operation deadlines, and layered outcomes | kernel IPC implementation and boot smoke test |
| 3 | NaoIDL parser, explicit UUID/IDs, canonical little-endian codec, bounded values, resource metadata callbacks (binding/scope/rights), build-local ABI manifest, compatibility checker, and typed client/server bindings with structural-violation disconnect hooks | `system_idl_test`, metadata-scope regression, generated binding contract, deterministic generation, output-location check, and build-local compatibility gate |
| 4 | TTY/PTY control exposed as typed KernelView methods; stream bytes remain a Stream capability; mlibc termios/PTY calls use the typed control path | BusyBox/userland build and headless serial boot |
| 5 | Native spawn creates a deferred child with an empty capability table and transfers only a bootstrap channel endpoint; mlibc sends root/cwd/ServiceDirectory/stdio capabilities over that channel and owns the POSIX fd table, including fd flags and open-state policy. Process capabilities retain child lifetime and expose typed wait/inspect calls; the compatibility fork/exec path remains available. | headless init boot exercises native `posix_spawn`, ServiceDirectory register/resolve/unregister, IDL-backed waitpid, ABI/bootstrap contract tests, and mlibc build |
| 6 | MemoryObject and shared-ring typed KernelView object calls with bounded limits and protocol rights; native VFS File/Directory adapters use typed methods | kernel build, ABI tests and headless boot |
| 7 | Legacy kernel file/object table, ioctl context/switches, direct native fd syscalls, and BusyBox raw ioctl dependency removed | source audit, full build and headless QEMU |

Every phase is required for the current acceptance target. A phase is not
considered verified merely because its headers exist: the repository tests,
the kernel build, and the serial boot smoke test must remain green together.

## Native mappings

The following mappings are the intended end state:

| Former surface | Native replacement |
| --- | --- |
| `open/read/write/pread/pwrite/lseek` | mlibc fd binding dispatching to File/Stream typed methods |
| `poll/select` | `na_handle_wait_many` over capability signals |
| `ioctl(TC* / TIO*)` | typed TTY control methods acquired from a Stream capability |
| kernel `file_desc` table | private mlibc fd table containing process-local native handles |
| process root/cwd in the resource table | process path context initialized by bootstrap |
| large or high-rate buffers | MemoryObject or shared ring |
| kernel VFS ioctl switch | native File/Directory protocol methods |
| child PID identity | process-local Process handle; `waitpid(pid > 0)` is an mlibc adapter over `Process.wait`, while aggregate waits use `Process.wait_children` |

POSIX names remain only as mlibc compatibility APIs. Native code must not
construct method ordinals or issue old syscall numbers directly. `waitpid(-1)`
and process-group filters retain POSIX aggregate-wait semantics through the
typed `Process.wait_children` method; there is no legacy child-wait syscall
fallback.

## Verification loop

```sh
ninja -C naos/libc/mlibc/build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DFREELIBCXX_TEST=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
timeout 35s python3 util/run.py q --iso -n
```

The timeout is expected because the booted system keeps running. The serial
log must reach `[info] init task running.` and contain no kernel panic, page
fault, capability leak, protocol violation, or usercopy failure. `-n` is
deliberate: QEMU is always run without a window for this project.
