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
set(NAOS_RUST_TLS_PROBE_LIB ${NAOS_RUST_TARGET_DIR}/x86_64-unknown-naos/release/librust_tls_probe.a)
set(NAOS_RUST_MAP ${CMAKE_CURRENT_BINARY_DIR}/rust-smoke.map)
set(NAOS_RUST_INSTALL ${ROOT_FS_DIR}/bin/rust-smoke-suite)
set(NAOS_RUST_FORK_DIR ${PROJECT_SOURCE_DIR}/naos/third_party/rust-naos)
message(STATUS "Using Rust source submodule ${NAOS_RUST_FORK_DIR}")
file(GLOB_RECURSE NAOS_RUST_FORK_LIBRARY_SOURCES CONFIGURE_DEPENDS
    ${NAOS_RUST_FORK_DIR}/library/alloc/*
    ${NAOS_RUST_FORK_DIR}/library/backtrace/*
    ${NAOS_RUST_FORK_DIR}/library/compiler-builtins/*
    ${NAOS_RUST_FORK_DIR}/library/core/*
    ${NAOS_RUST_FORK_DIR}/library/portable-simd/*
    ${NAOS_RUST_FORK_DIR}/library/std/*
    ${NAOS_RUST_FORK_DIR}/library/stdarch/*)

set(NAOS_RUST_SOURCES
    ${NAOS_RUST_ROOT}/.cargo/config.toml
    ${PROJECT_SOURCE_DIR}/Cargo.toml
    ${PROJECT_SOURCE_DIR}/Cargo.lock
    ${NAOS_RUST_ROOT}/link.ld
    ${NAOS_RUST_ROOT}/rust-toolchain.toml
    ${NAOS_RUST_ROOT}/targets/x86_64-unknown-naos.json
    ${NAOS_RUST_ROOT}/naos-sys/Cargo.toml
    ${NAOS_RUST_ROOT}/naos-sys/src/lib.rs
    ${NAOS_RUST_ROOT}/naos-sys/src/syscalls.S
    ${NAOS_RUST_ROOT}/naos-sys/tests/abi_layout.rs
    ${NAOS_RUST_ROOT}/naos-runtime/Cargo.toml
    ${NAOS_RUST_ROOT}/naos-runtime/src/allocator.rs
    ${NAOS_RUST_ROOT}/naos-runtime/src/thread.rs
    ${NAOS_RUST_ROOT}/naos-runtime/src/memory.S
    ${NAOS_RUST_ROOT}/naos-runtime/src/lib.rs
    ${NAOS_RUST_ROOT}/naos-runtime/src/start.S
    ${NAOS_RUST_ROOT}/naos-idl/Cargo.toml
    ${NAOS_RUST_ROOT}/naos-idl/build.rs
    ${NAOS_RUST_ROOT}/naos-idl/src/lib.rs
    ${PROJECT_SOURCE_DIR}/idl/templates/rust/naoidl/binding.rs.htt
    ${PROJECT_SOURCE_DIR}/idl/templates/rust/naoidl/index.rs.htt
    ${NAOS_IDL_COMPILER}
    ${PROJECT_SOURCE_DIR}/doc/examples/echo.naidl
    ${NAOS_RUST_ROOT}/rust-std-smoke/Cargo.toml
    ${NAOS_RUST_ROOT}/rust-std-smoke/src/main.rs
    ${NAOS_RUST_ROOT}/rust-std-smoke/src/native_smoke.rs
    ${NAOS_RUST_ROOT}/rust-tls-probe/Cargo.toml
    ${NAOS_RUST_ROOT}/rust-tls-probe/src/lib.rs
    ${NAOS_RUST_ROOT}/naos-idl/src/lib.rs)
file(GLOB NAOS_RUST_IDL_SCHEMAS CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/doc/examples/*.naidl
    ${PROJECT_SOURCE_DIR}/idl/system/*.naidl)
list(APPEND NAOS_RUST_SOURCES ${NAOS_RUST_IDL_SCHEMAS})

add_custom_command(
    OUTPUT ${NAOS_RUST_BINARY} ${NAOS_RUST_TLS_PROBE_LIB} ${NAOS_RUST_MAP}
    COMMAND ${CMAKE_COMMAND} -E env
            "RUSTC=${NAOS_RUSTC_EXECUTABLE}"
            "RUSTFLAGS=-C link-arg=-Wl,-Map,${NAOS_RUST_MAP} -C link-arg=-Wl,-T,${NAOS_RUST_ROOT}/link.ld"
            "NAOS_IDL_COMPILER=${NAOS_IDL_COMPILER}"
            "NAOS_PYTHON=${Python3_EXECUTABLE}"
            ${NAOS_CARGO_EXECUTABLE} build
            --manifest-path ${PROJECT_SOURCE_DIR}/Cargo.toml
            --locked
            --release
            --package rust-smoke-suite
            --package rust-tls-probe
            --target ${NAOS_RUST_TARGET_JSON}
            --target-dir ${NAOS_RUST_TARGET_DIR}
            -Z build-std=std,panic_abort
            -Z json-target-spec
    COMMAND ${CMAKE_COMMAND} -E touch ${NAOS_RUST_MAP}
    DEPENDS ${NAOS_RUST_SOURCES}
            ${PROJECT_SOURCE_DIR}/Cargo.lock ${NAOS_RUST_ROOT}/link.ld
    WORKING_DIRECTORY ${NAOS_RUST_ROOT}
    VERBATIM)

add_custom_command(
    OUTPUT ${NAOS_RUST_INSTALL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy ${NAOS_RUST_BINARY} ${NAOS_RUST_INSTALL}
    DEPENDS ${NAOS_RUST_BINARY}
    VERBATIM)

add_custom_target(naos_rust_smoke ALL DEPENDS ${NAOS_RUST_INSTALL})

add_library(naos_rust_tls_probe STATIC IMPORTED GLOBAL)
set_target_properties(naos_rust_tls_probe PROPERTIES IMPORTED_LOCATION ${NAOS_RUST_TLS_PROBE_LIB})
add_dependencies(naos_rust_tls_probe naos_rust_smoke)

add_test(
    NAME rust_native_elf_contract_test
    COMMAND ${CMAKE_COMMAND}
            -DIMAGE=${NAOS_RUST_BINARY}
            -DTLS_IMAGE=${NAOS_RUST_BINARY}
            -DMAP=${NAOS_RUST_MAP}
            -P ${PROJECT_SOURCE_DIR}/tests/rust_native_elf_contract.cmake)
