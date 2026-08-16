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

Use `-DCMAKE_BUILD_TYPE=Release` for an optimized kernel. Run the ISO in QEMU with `python3 util/run.py q --iso`; for a disk image, mount it first with `python3 util/disk.py mount`, then run `python3 util/run.py q`. Use `--uefi` when testing UEFI. Kernel serial output is written to `run/kernel_out.log`.

## Coding Style & Naming Conventions

Use `.clang-format`: four-space indentation, spaces only, a 120-column limit, and braces on their own lines. Run `clang-format -i path/to/file.cc` on changed C/C++ files. Follow nearby lower_snake_case names for files, functions, variables, and types; preserve established hardware acronyms and constants. Keep freestanding constraints in mind: avoid exceptions, RTTI, and hosted-library assumptions in kernel code.

## Testing Guidelines

There is no standalone kernel unit-test suite or repository-wide coverage threshold. Every change should at least compile and boot in an appropriate emulator, with relevant serial output checked. When changing `freelibcxx`, configure with `-DFREELIBCXX_TEST=ON` and run `ctest --test-dir build`; its Catch2 tests use lower_snake_case source names.

Do not use Python tests that read, scan, or parse C++ source code. Test C++ behavior with real C++ tests, compilation, and appropriate emulator execution instead.

## Commit & Pull Request Guidelines

Prefer concise imperative subjects using the existing Conventional Commit style, such as `feat: add ...`, `fix: ...`, or `docs: ...`. PRs should describe the behavior change, affected architecture or subsystem, exact build/test commands, and emulator results. Include serial logs or screenshots for boot, driver, or UI changes, and call out any submodule pointer updates. Do not commit generated build products, disk images, or logs.

## Configuration & Safety

Review compiler paths in `naos/libc/mlibc/cross_file.txt` and the OVMF path in `util/run.py` for your machine. Disk mounting and boot-image creation can require elevated privileges; verify image and mount paths before running those commands.
