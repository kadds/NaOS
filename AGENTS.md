# Repository Guidelines

## Project Structure & Module Organization

NaOS is a freestanding C++20 operating system targeting x86-64. Kernel sources are in `naos/src/kernel`, with public kernel headers in `naos/includes/kernel`; assembly and architecture-specific code live alongside their related kernel modules. Userland programs are under `naos/src/usr` (`init` and `nanobox`). `naos/freelibcxx`, `naos/libc/mlibc`, and `naos/src/acpica` are submodules. Emulator configuration and fake root files are in `run/`, Python build/run helpers are in `util/`, and documentation assets are in `doc/`. Treat `build/` and generated files under `run/image/` as disposable.

See [`doc/ARCHITECTURE.md`](doc/ARCHITECTURE.md) for the kernel/userland boundaries and the build-to-boot image flow.

## Build, Test, and Development Commands

Initialize dependencies and build the libc before configuring the main project:

```bash
git submodule update --init --recursive
meson setup naos/libc/mlibc/build --cross-file naos/libc/mlibc/cross_file.txt
ninja -C naos/libc/mlibc/build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DFREELIBCXX_TEST=OFF
cmake --build build -j
```

Incremental-build note: `naos/libc/mlibc` is an external submodule exposed to
CMake as an imported static library, so changes there require rebuilding it
explicitly with `ninja -C naos/libc/mlibc/build` before relinking userland.
Building only individual targets such as `init` or `kernel` updates their
direct binaries but does not refresh `build/bin/system/rfsimg`; build the
`make-rfs-image` target (or the complete default target) before boot testing.
`python3 util/run.py q --iso` only packages the already-built kernel and
rootfs image into an ISO; it does not compile or repack userland.

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
    --set build.build-dir="${PWD}/build/rust-naos" \
    --set build.jobs=2 --set llvm.download-ci-llvm=true \
    --set rust.deny-warnings=false
python3 naos/third_party/rust-naos/x.py build --stage 1 library/std
python3 naos/third_party/rust-naos/x.py build --stage 1 src/tools/cargo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
    -DNAOS_RUST_TOOLCHAIN_ROOT="${PWD}/build/rust-naos/x86_64-unknown-linux-gnu/stage1" \
    -DNAOS_CARGO_EXECUTABLE="${PWD}/build/rust-naos/x86_64-unknown-linux-gnu/stage1-tools-bin/cargo"
cmake --build build --target naos_rust_smoke -j
```

`configure` creates the machine-local, ignored `bootstrap.toml` inside the
Rust submodule. `naos_rust_smoke` builds and installs the single Rust smoke
ELF, `/bin/rust-smoke-suite`; it includes native bootstrap, alloc, NaoIDL,
compiler TLS/thread, and NaOS `std` checks. `rust-tls-probe` is only a static
library consumed by the C++ TLS smoke and is not a userland executable.
The source directory may retain its historical `rust-std-smoke` path, but its
Cargo package and installed executable are both named `rust-smoke-suite`.
`naos-idl/build.rs` discovers every `.naidl` under `doc/examples` and
`idl/system` by default, writes bindings and the module index under Cargo's
`OUT_DIR`, and compiles them through the Rust templates. To check the complete
Rust IDL surface directly:

```bash
build/rust-naos/x86_64-unknown-linux-gnu/stage1-tools-bin/cargo test \
    --locked -p naos-idl --features alloc
```

Set `NAOS_IDL_SCHEMAS` to a semicolon-separated schema list when a small
consumer-specific binding set is required.

Use `-DCMAKE_BUILD_TYPE=Release` for an optimized kernel. Run the ISO in QEMU with `python3 util/run.py q --iso`; for a disk image, mount it first with `python3 util/disk.py mount`, then run `python3 util/run.py q`. Use `--uefi` when testing UEFI. Kernel serial output is written to `run/kernel_out.log`.

## Coding Style & Naming Conventions

Use `.clang-format`: four-space indentation, spaces only, a 120-column limit, and braces on their own lines. Run `clang-format -i path/to/file.cc` on changed C/C++ files. Follow nearby lower_snake_case names for files, functions, variables, and types; preserve established hardware acronyms and constants. Keep freestanding constraints in mind: avoid exceptions, RTTI, and hosted-library assumptions in kernel code.

## Testing Guidelines

There is no standalone kernel unit-test suite or repository-wide coverage threshold. Every change should at least compile and boot in an appropriate emulator, with relevant serial output checked. When changing `freelibcxx`, configure with `-DFREELIBCXX_TEST=ON` and run `ctest --test-dir build`; its Catch2 tests use lower_snake_case source names.

Do not use Python tests that read, scan, or parse C++ source code. Test C++ behavior with real C++ tests, compilation, and appropriate emulator execution instead.

Keep tests and smoke runners out of `init`: normal init must only start and supervise
system services and the interactive shell. Put manual test invocations in the
submitted `/etc/init.sh` and trigger them only when that opt-in file is present,
so a failing test cannot affect the normal boot path.

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

## Commit & Pull Request Guidelines

Prefer concise imperative subjects using the existing Conventional Commit style, such as `feat: add ...`, `fix: ...`, or `docs: ...`. PRs should describe the behavior change, affected architecture or subsystem, exact build/test commands, and emulator results. Include serial logs or screenshots for boot, driver, or UI changes, and call out any submodule pointer updates. Do not commit generated build products, disk images, or logs.

## Configuration & Safety

Review compiler paths in `naos/libc/mlibc/cross_file.txt` and the OVMF path in `util/run.py` for your machine. Disk mounting and boot-image creation can require elevated privileges; verify image and mount paths before running those commands.
