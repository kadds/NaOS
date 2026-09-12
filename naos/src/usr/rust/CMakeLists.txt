set(NAOS_RUST_TOOLCHAIN_ROOT "" CACHE PATH
    "Root of a locally built NaOS Rust toolchain (for example, build/rust-naos/x86_64-unknown-linux-gnu/stage1)")
set(NAOS_CARGO_EXECUTABLE "" CACHE FILEPATH
    "Cargo executable used to drive the NaOS Rust toolchain")

if(NAOS_RUST_TOOLCHAIN_ROOT)
    set(NAOS_RUSTC_EXECUTABLE "${NAOS_RUST_TOOLCHAIN_ROOT}/bin/rustc")
    if(NOT EXISTS "${NAOS_RUSTC_EXECUTABLE}")
        message(FATAL_ERROR
            "NAOS_RUST_TOOLCHAIN_ROOT does not contain bin/rustc: ${NAOS_RUST_TOOLCHAIN_ROOT}")
    endif()
    message(STATUS "Using locally built NaOS rustc ${NAOS_RUSTC_EXECUTABLE}")
else()
    find_program(NAOS_RUSTC_EXECUTABLE rustc REQUIRED)
endif()

if(NOT NAOS_CARGO_EXECUTABLE)
    find_program(NAOS_CARGO_EXECUTABLE cargo REQUIRED)
endif()
message(STATUS "Using Cargo ${NAOS_CARGO_EXECUTABLE}")
find_program(NAOS_BASH_EXECUTABLE bash REQUIRED)

set(NAOS_RUST_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(NAOS_RUST_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo-target)
set(NAOS_RUST_TARGET_JSON ${NAOS_RUST_ROOT}/targets/x86_64-unknown-naos.json)
set(NAOS_RUST_BINARY ${NAOS_RUST_TARGET_DIR}/x86_64-unknown-naos/release/rust-smoke-suite)
set(NAOS_VFS_BINARY ${NAOS_RUST_TARGET_DIR}/x86_64-unknown-naos/release/vfsd)
set(NAOS_RAMDISKD_BINARY ${NAOS_RUST_TARGET_DIR}/x86_64-unknown-naos/release/ramdiskd)
set(NAOS_EXFATD_BINARY ${NAOS_RUST_TARGET_DIR}/x86_64-unknown-naos/release/exfatd)
set(NAOS_RUST_TLS_PROBE_LIB ${NAOS_RUST_TARGET_DIR}/x86_64-unknown-naos/release/librust_tls_probe.a)
set(NAOS_RUST_MAP ${CMAKE_CURRENT_BINARY_DIR}/rust-smoke.map)
set(NAOS_RUST_BUILD_STAMP ${CMAKE_CURRENT_BINARY_DIR}/rust-build.stamp)
set(NAOS_RUST_INSTALL ${ROOT_FS_DIR}/bin/rust-smoke-suite)
set(NAOS_VFS_INSTALL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/vfsd)
set(NAOS_RAMDISKD_INSTALL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/ramdiskd)
set(NAOS_EXFATD_INSTALL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/exfatd)
set(NAOS_ROOTFSD_INSTALL ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/rootfsd)
set(NAOS_RUST_FORK_DIR ${PROJECT_SOURCE_DIR}/naos/third_party/rust-naos)
set(NAOS_RUST_BUILD_HELPER ${PROJECT_SOURCE_DIR}/util/naos_rust_build.py)
set(NAOS_RUST_FINGERPRINT ${NAOS_RUST_TARGET_DIR}/.naos-rust-input.sha256)
message(STATUS "Using Rust source submodule ${NAOS_RUST_FORK_DIR}")
file(GLOB_RECURSE NAOS_RUST_FORK_LIBRARY_SOURCES CONFIGURE_DEPENDS
    ${NAOS_RUST_FORK_DIR}/library/alloc/*
    ${NAOS_RUST_FORK_DIR}/library/backtrace/*
    ${NAOS_RUST_FORK_DIR}/library/compiler-builtins/*
    ${NAOS_RUST_FORK_DIR}/library/core/*
    ${NAOS_RUST_FORK_DIR}/library/proc_macro/*
    ${NAOS_RUST_FORK_DIR}/library/portable-simd/*
    ${NAOS_RUST_FORK_DIR}/library/std/*
    ${NAOS_RUST_FORK_DIR}/library/stdarch/*)

file(GLOB_RECURSE NAOS_RUST_CRATE_SOURCES CONFIGURE_DEPENDS
    "${NAOS_RUST_ROOT}/*/src/*.rs"
    "${NAOS_RUST_ROOT}/*/src/*.S"
    "${NAOS_RUST_ROOT}/*/tests/*.rs")
file(GLOB NAOS_RUST_CRATE_MANIFESTS CONFIGURE_DEPENDS
    "${NAOS_RUST_ROOT}/*/Cargo.toml")
file(GLOB_RECURSE NAOS_RUST_CRATE_BUILD_INPUTS CONFIGURE_DEPENDS
    "${NAOS_RUST_ROOT}/*/build.rs"
    "${NAOS_RUST_ROOT}/*/Cargo.lock")

file(GLOB_RECURSE NAOS_IPC_CORE_RUST_BUILD_INPUTS CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/naos/libipc/include/*"
    "${PROJECT_SOURCE_DIR}/naos/libipc/src/*")

# The workspace patches crates.io Tokio and Mio with these source trees. They
# are inputs to the Cargo command above, so CMake must watch their manifests,
# build scripts, and Rust sources as well; otherwise editing a patched async
# runtime leaves the installed service binaries incorrectly up to date.
file(GLOB_RECURSE NAOS_RUST_ASYNC_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/naos/third_party/mio/*.rs"
    "${PROJECT_SOURCE_DIR}/naos/third_party/mio/*.toml"
    "${PROJECT_SOURCE_DIR}/naos/third_party/mio/*.lock"
    "${PROJECT_SOURCE_DIR}/naos/third_party/tokio/*.rs"
    "${PROJECT_SOURCE_DIR}/naos/third_party/tokio/*.toml"
    "${PROJECT_SOURCE_DIR}/naos/third_party/tokio/*.lock")

# Cargo owns the Rust module graph.  CMake only needs a recursive watch list
# to re-run this custom command when a crate file is added or edited; keep the
# list itself free of one-entry-per-source-file maintenance.
set(NAOS_RUST_SOURCES
    ${NAOS_RUST_ROOT}/.cargo/config.toml
    ${PROJECT_SOURCE_DIR}/Cargo.toml
    ${PROJECT_SOURCE_DIR}/Cargo.lock
    ${NAOS_RUST_ROOT}/link.ld
    ${NAOS_RUST_ROOT}/rust-toolchain.toml
    ${NAOS_RUST_ROOT}/targets/x86_64-unknown-naos.json
    ${NAOS_RUST_ROOT}/naos-idl/build.rs
    ${PROJECT_SOURCE_DIR}/idl/templates/rust/naoidl/binding.rs.htt
    ${PROJECT_SOURCE_DIR}/idl/templates/rust/naoidl/index.rs.htt
    ${NAOS_IDL_COMPILER}
    ${PROJECT_SOURCE_DIR}/doc/examples/echo.naidl
    ${NAOS_RUST_BUILD_HELPER}
    ${NAOS_RUST_CRATE_MANIFESTS})
list(APPEND NAOS_RUST_SOURCES
    ${NAOS_RUST_CRATE_SOURCES}
    ${NAOS_RUST_CRATE_BUILD_INPUTS}
    ${NAOS_IPC_CORE_RUST_BUILD_INPUTS}
    ${NAOS_RUST_FORK_LIBRARY_SOURCES}
    ${NAOS_RUST_ASYNC_RUNTIME_SOURCES})
file(GLOB NAOS_RUST_IDL_SCHEMAS CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/doc/examples/*.naidl
    ${PROJECT_SOURCE_DIR}/idl/system/*.naidl)
list(APPEND NAOS_RUST_SOURCES ${NAOS_RUST_IDL_SCHEMAS})

add_custom_command(
    OUTPUT ${NAOS_RUST_BUILD_STAMP}
    BYPRODUCTS ${NAOS_RUST_BINARY} ${NAOS_VFS_BINARY} ${NAOS_RAMDISKD_BINARY} ${NAOS_EXFATD_BINARY}
               ${NAOS_RUST_TLS_PROBE_LIB} ${NAOS_RUST_MAP}
    # A failed Cargo invocation must not leave an older stamp looking newer
    # than the source that caused the rebuild.  Remove it before invoking the
    # wrapper; the successful command below recreates it.
    COMMAND ${CMAKE_COMMAND} -E rm -f ${NAOS_RUST_BUILD_STAMP}
    COMMAND ${CMAKE_COMMAND} -E env
            "RUSTC=${NAOS_RUSTC_EXECUTABLE}"
            # The locally built stage1 rustc reports a release channel but
            # must accept -Z build-std flags for the custom target.
            "RUSTC_BOOTSTRAP=1"
            "RUSTFLAGS=-C link-arg=-Wl,-Map,${NAOS_RUST_MAP} -C link-arg=-Wl,-T,${NAOS_RUST_ROOT}/link.ld"
            "NAOS_IDL_COMPILER=${NAOS_IDL_COMPILER}"
            "NAOS_PYTHON=${Python3_EXECUTABLE}"
            ${Python3_EXECUTABLE} ${NAOS_RUST_BUILD_HELPER}
            --workspace ${PROJECT_SOURCE_DIR}
            --target-dir ${NAOS_RUST_TARGET_DIR}
            --fingerprint-file ${NAOS_RUST_FINGERPRINT}
            --input-root ${NAOS_RUST_ROOT}
            --input-root ${PROJECT_SOURCE_DIR}/idl
            --input-root ${PROJECT_SOURCE_DIR}/doc/examples
            --input-root ${NAOS_RUST_FORK_DIR}/library
            --input-root ${PROJECT_SOURCE_DIR}/naos/third_party/mio
            --input-root ${PROJECT_SOURCE_DIR}/naos/third_party/tokio
            -- ${NAOS_CARGO_EXECUTABLE} build
            --manifest-path ${PROJECT_SOURCE_DIR}/Cargo.toml
            --locked
            --release
            --package rust-smoke-suite
            --package rust-tls-probe
            --package vfsd
            --package ramdiskd
            --package exfatd
            --target ${NAOS_RUST_TARGET_JSON}
            --target-dir ${NAOS_RUST_TARGET_DIR}
            -Z build-std=std,panic_abort,proc_macro
            -Z json-target-spec
    COMMAND ${CMAKE_COMMAND} -E rm -f
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/naos-vfsd
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/naos-ramdiskd
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/naos-exfatd
    COMMAND ${CMAKE_COMMAND} -E touch ${NAOS_RUST_MAP}
    COMMAND ${CMAKE_COMMAND} -E touch ${NAOS_RUST_BUILD_STAMP}
    DEPENDS ${NAOS_RUST_SOURCES} ${NAOS_RUST_BUILD_HELPER}
            ${PROJECT_SOURCE_DIR}/Cargo.lock ${NAOS_RUST_ROOT}/link.ld
    WORKING_DIRECTORY ${NAOS_RUST_ROOT}
    VERBATIM)

# Keep the Cargo invocation attached to one CMake target. Referencing the
# stamp file directly from several recursive Make sub-builds makes each
# sub-build believe it owns the recipe and can launch Cargo concurrently.
add_custom_target(naos_rust_build DEPENDS ${NAOS_RUST_BUILD_STAMP})

add_custom_command(
    OUTPUT ${NAOS_RUST_INSTALL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy ${NAOS_RUST_BINARY} ${NAOS_RUST_INSTALL}
    DEPENDS naos_rust_build ${NAOS_RUST_BINARY}
    VERBATIM)

add_custom_command(
    OUTPUT ${NAOS_VFS_INSTALL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system
    COMMAND ${CMAKE_COMMAND} -E copy ${NAOS_VFS_BINARY} ${NAOS_VFS_INSTALL}
    DEPENDS naos_rust_build ${NAOS_VFS_BINARY}
    VERBATIM)

add_custom_command(
    OUTPUT ${NAOS_RAMDISKD_INSTALL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system
    COMMAND ${CMAKE_COMMAND} -E copy ${NAOS_RAMDISKD_BINARY} ${NAOS_RAMDISKD_INSTALL}
    DEPENDS naos_rust_build ${NAOS_RAMDISKD_BINARY}
    VERBATIM)

add_custom_command(
    OUTPUT ${NAOS_EXFATD_INSTALL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system
    COMMAND ${CMAKE_COMMAND} -E copy ${NAOS_EXFATD_BINARY} ${NAOS_EXFATD_INSTALL}
    DEPENDS naos_rust_build ${NAOS_EXFATD_BINARY}
    VERBATIM)

add_custom_command(
    OUTPUT ${NAOS_ROOTFSD_INSTALL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system
    COMMAND ${CMAKE_COMMAND} -E copy ${NAOS_EXFATD_BINARY} ${NAOS_ROOTFSD_INSTALL}
    DEPENDS naos_rust_build ${NAOS_EXFATD_BINARY}
    VERBATIM)


# The early-service file daemon ships on the boot media as its own Multiboot
# module (USERSPACE_FILESYSTEM_ADR §5.1); run.py picks it up from here.
add_custom_target(vfsd ALL DEPENDS ${NAOS_VFS_INSTALL})

# The optional volume-manager daemon ships on the boot media as its own
# Multiboot module (VFS ADR §7 item 1); grub.cfg and run.py pick it up from
# here when present.
add_custom_target(ramdiskd ALL DEPENDS ${NAOS_RAMDISKD_INSTALL})

# `exfatd` is the concrete implementation binary.  `rootfsd` is the fixed
# Multiboot authority token and is staged as a separately named image so the
# kernel/userland handoff does not depend on an archive path.
add_custom_target(exfatd ALL DEPENDS ${NAOS_EXFATD_INSTALL})
add_custom_target(rootfsd ALL DEPENDS ${NAOS_ROOTFSD_INSTALL})

add_custom_target(naos_rust_smoke ALL DEPENDS ${NAOS_RUST_INSTALL})

add_library(naos_rust_tls_probe STATIC IMPORTED GLOBAL)
set_target_properties(naos_rust_tls_probe PROPERTIES IMPORTED_LOCATION ${NAOS_RUST_TLS_PROBE_LIB})
add_dependencies(naos_rust_tls_probe naos_rust_build)

add_test(
    NAME rust_native_elf_contract_test
    COMMAND ${CMAKE_COMMAND}
            -DIMAGE=${NAOS_RUST_BINARY}
            -DTLS_IMAGE=${NAOS_RUST_BINARY}
            -DMAP=${NAOS_RUST_MAP}
            -P ${PROJECT_SOURCE_DIR}/tests/rust_native_elf_contract.cmake)
