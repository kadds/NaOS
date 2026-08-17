find_program(NAOS_CARGO_EXECUTABLE cargo REQUIRED)

set(NAOS_RUST_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(NAOS_RUST_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo-target)
set(NAOS_RUST_TARGET_JSON ${NAOS_RUST_ROOT}/targets/x86_64-unknown-naos.json)
set(NAOS_RUST_BINARY ${NAOS_RUST_TARGET_DIR}/x86_64-unknown-naos/release/rust-smoke)
set(NAOS_RUST_MAP ${CMAKE_CURRENT_BINARY_DIR}/rust-smoke.map)
set(NAOS_RUST_INSTALL ${ROOT_FS_DIR}/bin/rust-smoke)

set(NAOS_RUST_SOURCES
    ${NAOS_RUST_ROOT}/.cargo/config.toml
    ${NAOS_RUST_ROOT}/Cargo.toml
    ${NAOS_RUST_ROOT}/Cargo.lock
    ${NAOS_RUST_ROOT}/link.ld
    ${NAOS_RUST_ROOT}/rust-toolchain.toml
    ${NAOS_RUST_ROOT}/targets/x86_64-unknown-naos.json
    ${NAOS_RUST_ROOT}/naos-sys/Cargo.toml
    ${NAOS_RUST_ROOT}/naos-sys/src/lib.rs
    ${NAOS_RUST_ROOT}/naos-sys/src/syscalls.S
    ${NAOS_RUST_ROOT}/naos-sys/tests/abi_layout.rs
    ${NAOS_RUST_ROOT}/naos-runtime/Cargo.toml
    ${NAOS_RUST_ROOT}/naos-runtime/src/lib.rs
    ${NAOS_RUST_ROOT}/naos-runtime/src/start.S
    ${NAOS_RUST_ROOT}/rust-smoke/Cargo.toml
    ${NAOS_RUST_ROOT}/rust-smoke/src/main.rs)

add_custom_command(
    OUTPUT ${NAOS_RUST_BINARY} ${NAOS_RUST_MAP}
    COMMAND ${CMAKE_COMMAND} -E env
            "RUSTFLAGS=-C link-arg=-Wl,-Map,${NAOS_RUST_MAP}"
            ${NAOS_CARGO_EXECUTABLE} build
            --locked
            --release
            --package rust-smoke
            --target ${NAOS_RUST_TARGET_JSON}
            --target-dir ${NAOS_RUST_TARGET_DIR}
            -Z build-std=core,compiler_builtins
            -Z json-target-spec
    COMMAND ${CMAKE_COMMAND} -E touch ${NAOS_RUST_MAP}
    DEPENDS ${NAOS_RUST_SOURCES} ${NAOS_RUST_ROOT}/Cargo.lock ${NAOS_RUST_ROOT}/link.ld
    WORKING_DIRECTORY ${NAOS_RUST_ROOT}
    VERBATIM)

add_custom_command(
    OUTPUT ${NAOS_RUST_INSTALL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    COMMAND ${CMAKE_COMMAND} -E copy ${NAOS_RUST_BINARY} ${NAOS_RUST_INSTALL}
    DEPENDS ${NAOS_RUST_BINARY}
    VERBATIM)

add_custom_target(naos_rust_smoke ALL DEPENDS ${NAOS_RUST_INSTALL})

add_test(
    NAME rust_native_elf_contract_test
    COMMAND ${CMAKE_COMMAND}
            -DIMAGE=${NAOS_RUST_BINARY}
            -DMAP=${NAOS_RUST_MAP}
            -P ${PROJECT_SOURCE_DIR}/tests/rust_native_elf_contract.cmake)
