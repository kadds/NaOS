# Repository Guidelines

## Project Structure & Module Organization

NaOS is a freestanding C++20 operating system targeting x86-64. Kernel sources are in `naos/src/kernel`, with public kernel headers in `naos/includes/kernel`; assembly and architecture-specific code live alongside their related kernel modules. Userland programs are under `naos/src/usr` (`init` and `nanobox`). `naos/freelibcxx`, `naos/libc/mlibc`, and `naos/src/acpica` are submodules. Emulator configuration and fake root files are in `run/`, Python build/run helpers are in `util/`, and documentation assets are in `doc/`. Treat each `build-*` directory and its generated contents as disposable; `run/` is source configuration only.

See [`doc/ARCHITECTURE.md`](doc/ARCHITECTURE.md) for the kernel/userland boundaries and the build-to-boot image flow.

## Build, Test, and Development Commands

Initialize dependencies and build the libc before configuring the main project:

```bash
git submodule update --init --recursive
meson setup naos/libc/mlibc/build --cross-file naos/libc/mlibc/cross_file.txt
ninja -C naos/libc/mlibc/build
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DFREELIBCXX_TEST=OFF
cmake --build build-debug -j
```

Incremental-build note: `naos/libc/mlibc` is an external submodule exposed to
CMake as an imported static library, so changes there require rebuilding it
explicitly with `ninja -C naos/libc/mlibc/build` before relinking userland.
Building only individual targets such as `init` or `kernel` updates their
direct binaries but does not refresh `<build-dir>/bin/system/root.img`; build
the `make-root-image` target (or the complete default target) before boot
testing. `python3 util/run.py --build-dir build-debug q --iso` only packages
the already-built kernel and root image into an ISO; it does not compile or
repack userland.

### Rust userland toolchain

Rust userland must be built with the pinned Rust fork submodule at
`naos/third_party/rust-naos`, not with the host `rust-src` or the removed
`std-overlay`. The fork commit is pinned by the parent gitlink. Its `1.97.1`
source requires the `1.96.0` bootstrap compiler:

```bash
git submodule update --init --recursive
RUSTUP_DIST_SERVER=https://static.rust-lang.org \
    rustup toolchain install 1.96.0 --profile minimal
rustup_home_dir="$(rustup show home)"
bash naos/third_party/rust-naos/configure \
    --enable-local-rust \
    --local-rust-root="${rustup_home_dir}/toolchains/1.96.0-x86_64-unknown-linux-gnu" \
    --enable-local-rebuild --disable-extended \
    --set build.build-dir="${PWD}/build-debug/rust-naos" \
    --set build.jobs=2 --set llvm.download-ci-llvm=true \
    --set rust.deny-warnings=false
python3 naos/third_party/rust-naos/x.py build --stage 1 library/std
python3 naos/third_party/rust-naos/x.py build --stage 1 library/proc_macro
python3 naos/third_party/rust-naos/x.py build --stage 1 src/tools/cargo
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
    -DNAOS_RUST_TOOLCHAIN_ROOT="${PWD}/build-debug/rust-naos/x86_64-unknown-linux-gnu/stage1" \
    -DNAOS_CARGO_EXECUTABLE="${PWD}/build-debug/rust-naos/x86_64-unknown-linux-gnu/stage1-tools-bin/cargo"
cmake --build build-debug --target naos_rust_smoke -j
```

`configure` creates the machine-local, ignored `bootstrap.toml` inside the
Rust submodule. `naos_rust_smoke` builds and installs the single Rust smoke
ELF, `/bin/rust-smoke-suite`; it includes native bootstrap, alloc, NaoIDL,
compiler TLS/thread, and NaOS `std` checks. `rust-tls-probe` is only a static
library consumed by the C++ TLS smoke and is not a userland executable.
NaOS `std` is treated as a supported target by the Rust fork, so ordinary
third-party crates such as `byteorder` do not need `#![feature(restricted_std)]`.
CMake watches the Rust fork, crate sources, and patched Tokio/Mio trees; its
Rust build wrapper fingerprints those inputs and clears the matching Cargo
target directory when their contents change.
The source directory may retain its historical `rust-std-smoke` path, but its
Cargo package and installed executable are both named `rust-smoke-suite`.
`naos-idl/build.rs` discovers every `.naidl` under `doc/examples` and
`idl/system` by default, writes bindings and the module index under Cargo's
`OUT_DIR`, and compiles them through the Rust templates. To check the complete
Rust IDL surface directly:

```bash
build-debug/rust-naos/x86_64-unknown-linux-gnu/stage1-tools-bin/cargo test \
    --locked -p naos-idl --features alloc
```

Set `NAOS_IDL_SCHEMAS` to a semicolon-separated schema list when a small
consumer-specific binding set is required.

Kernel-provided userland resources must not be added as service-specific
bootstrap capabilities. Bootstrap is limited to the common ServiceDirectory,
stdio, and process-start contract; services acquire kernel-owned resources by
resolving their published URI through ServiceDirectory. When a resource is
needed during early boot, the kernel must publish it as a service before
starting the consumer. Do not add compatibility capability kinds or reserved
enum holes for unpublished designs.

Linux UDS is only the transport, not an authority substitute. The Linux
servicekit adapter must keep an authoritative resource table, accept only
identity-preserving or rights-attenuated descriptors, require `TRANSFER` for
resource arguments, and enforce binding/scope plus operation-specific rights
at request admission and again at data access. `SCM_RIGHTS` only transfers an
fd; it does not remove the need for these checks.

The reusable channel state machine lives in `naos/libipc` and its stable
boundary is the C header `naos/ipc_core.h`. Kernel code and Linux Rust code
must provide allocator, lock, notifier, clock, handle-table, and resource
callbacks through that ABI; they must not include the private C++ channel
implementation or duplicate queue/claim/commit logic. Kernel-only root,
capability, endpoint-object, resource-table, and wait-queue bookkeeping stays
in the kernel adapter. Linux IPC tests must exercise the C ABI directly and
cover the same FIFO, backpressure, close, cancellation, resource-transfer,
concurrent send/receive, and wait-set-invalidated behavior as the kernel
adapter.

Invocation lifecycle and result ownership follow the same boundary: the
transport-neutral implementation lives in `naos/libipc`, while kernel
capability/resource-table/protocol-queue/timer/wait-queue behavior remains in
the kernel adapter. Its C ABI must not expose capability objects or kernel
wait-queue types. The Linux Rust adapter must bind that ABI directly and test
reply ownership, result capacity, queued and dispatched cancellation, and
deadline outcomes against the same core.

Kernel readiness is exposed through epoll control/wait operations only; do not
add a kernel `wait_many` or select-style syscall. Tokio's NaOS Mio backend owns
the single runtime selector. Servicekit servers must attach their capability
registrations to Tokio's generic readiness set and must not create, wrap, or
re-export an epoll object. POSIX `poll`/`select` compatibility belongs in the
user-space mlibc adapter.

Use `-DCMAKE_BUILD_TYPE=Release` for an optimized kernel. Run the ISO in QEMU with `python3 util/run.py --build-dir build-debug q --iso`; for a prepared disk image, use `python3 util/disk.py --build-dir build-debug mount` to validate p1 and then `python3 util/run.py --build-dir build-debug q` (the latter refreshes `/boot` through `fstool`, without host mounts). Use `--uefi` when testing UEFI. Kernel serial output is written to `build-debug/kernel_out.log`. Agents must launch QEMU detached from the foreground and inspect the build-local log instead of occupying the terminal with the emulator.

## Coding Style & Naming Conventions

Use `.clang-format`: four-space indentation, spaces only, a 120-column limit, and braces on their own lines. Run `clang-format -i path/to/file.cc` on changed C/C++ files. Follow nearby lower_snake_case names for files, functions, variables, and types; preserve established hardware acronyms and constants. Keep freestanding constraints in mind: avoid exceptions, RTTI, and hosted-library assumptions in kernel code.

## Testing Guidelines

There is no standalone kernel unit-test suite or repository-wide coverage threshold. Every change should at least compile and boot in an appropriate emulator, with relevant serial output checked. When changing `freelibcxx`, configure with `-DFREELIBCXX_TEST=ON` and run `ctest --test-dir build-debug`; its Catch2 tests use lower_snake_case source names.

For cross-platform Rust services and shared protocol code, test in two stages:
first run the Linux/std/Tokio preflight with
`cargo test --locked --workspace`;
only after it passes, build the pinned custom NaOS toolchain and run the
relevant NaOS target/QEMU smoke. Linux is the fast check for shared codec,
state-machine, logging, transport, and data-plane behavior; it does not replace
NaOS-specific validation of capabilities, syscalls, custom `std`, linker entry,
or the NaOS reactor. Changes confined to a platform-specific adapter still
require that adapter's native test.

Do not use Python tests that read, scan, or parse C++ source code. Test C++ behavior with real C++ tests, compilation, and appropriate emulator execution instead.

All generated emulator state is owned by the selected build directory:
`<build-dir>/kernel_out.log`, `<build-dir>/qemu.log`, `<build-dir>/iso`,
`<build-dir>/image`, and `<build-dir>/boot.lock`. `util/run.py` acquires the
build-local lock before preparing images or starting QEMU. After a run, agents
must inspect the build-local `kernel_out.log` directly for boot markers and
errors. Do not invoke `qemu-system-*` directly or run QEMU in the foreground. If QEMU validations need to run in
parallel, configure a separate build directory for each run and use distinct
`--gdb-port` values because the default GDB port remains 1234.
Prefer code inspection and deterministic reproduction when diagnosing issues;
avoid repeatedly adding ad-hoc debug logs unless existing evidence is
insufficient.

Keep tests and smoke runners out of `init`: normal init must only start and supervise
system services and the interactive shell. Put manual test invocations in the
submitted `/etc/init.sh` and trigger them only when that opt-in file is present,
so a failing test cannot affect the normal boot path.

Normal service startup must never format, zero, or otherwise initialize a block
medium. Mounting an unformatted medium is an explicit startup failure; format
operations belong to an offline management command or an opt-in test only.
NaOS `MemoryObject` storage is a user-space data plane: after the kernel creates
or authorizes the object, userland must map it and copy bytes directly. Do not
use `MemoryObject.read/write` IPC as a buffer-fill path. Linux exposes the same
servicekit `MemoryObject` API; its private memfd/SCM_RIGHTS implementation must
not leak a `BulkRegion` or another platform-specific buffer type to a daemon.

`init` may run an optional `/etc/init.sh` exactly once after `ttyd` is ready and
before interactive-shell supervision starts. It must skip the script when either
`/etc/init.sh` or `/bin/sh` is absent, and a script failure must be logged without
turning the normal boot path into a test failure. Do not put test invocations back
into `init`; opt-in test runs belong in the submitted `/etc/init.sh`.
`tests/init.sh` is only the opt-in suite template and must not be copied into the
default fake rootfs unless that test run is intentional.

Every userland smoke binary must send each diagnostic/result message through both
the native `_s_log` path and stdout. Rust native smoke uses the bootstrap stdout
Stream; C/C++ smoke uses the mlibc stdout path. Do not make either channel the
sole source of smoke diagnostics.

The generic `util/run.py` launcher must not read or inspect serial-log strings.
It owns build-local image preparation, QEMU lifecycle, artifact checks, and
exit-status handling only. A boot or opt-in smoke test that needs a verdict must
use its own test-specific wrapper and inspect the selected build directory's
serial log there; do not add a generic marker-checking option to `run.py`.

## Commit & Pull Request Guidelines

Prefer concise imperative subjects using the existing Conventional Commit style, such as `feat: add ...`, `fix: ...`, or `docs: ...`. PRs should describe the behavior change, affected architecture or subsystem, exact build/test commands, and emulator results. Include serial logs or screenshots for boot, driver, or UI changes, and call out any submodule pointer updates. Do not commit generated build products, disk images, or logs.

## Configuration & Safety

Review compiler paths in `naos/libc/mlibc/cross_file.txt` and the OVMF path in `util/run.py` for your machine. Disk mounting and boot-image creation can require elevated privileges; verify image and mount paths before running those commands.
