find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(NAOS_CARGO_EXECUTABLE cargo REQUIRED)

file(GLOB NAOS_RUST_IDL_CPP_TEMPLATES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/idl/templates/cpp/naoidl/*.htt)

set(NAOS_FORMATTER_COMPILE_FAIL_BINARY_DIR ${CMAKE_BINARY_DIR}/formatter_compile_fail)
try_compile(
    NAOS_FORMATTER_REJECTED
    ${NAOS_FORMATTER_COMPILE_FAIL_BINARY_DIR}
    SOURCES ${PROJECT_SOURCE_DIR}/tests/formatter_compile_fail.cc
    CMAKE_FLAGS
        "-DCMAKE_CXX_STANDARD=23"
        "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    COMPILE_DEFINITIONS "-I${PROJECT_SOURCE_DIR}/naos/freelibcxx/include"
    OUTPUT_VARIABLE NAOS_FORMATTER_COMPILE_FAIL_OUTPUT)
if(NAOS_FORMATTER_REJECTED)
    message(FATAL_ERROR "formatter accepted an invalid format/type combination:\n${NAOS_FORMATTER_COMPILE_FAIL_OUTPUT}")
endif()

set(NAOS_KLOG_COMPILE_FAIL_BINARY_DIR ${CMAKE_BINARY_DIR}/klog_compile_fail)
try_compile(
    NAOS_KLOG_REJECTED
    ${NAOS_KLOG_COMPILE_FAIL_BINARY_DIR}
    SOURCES ${PROJECT_SOURCE_DIR}/tests/klog_compile_fail.cc
    CMAKE_FLAGS
        "-DCMAKE_CXX_STANDARD=23"
        "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
    COMPILE_DEFINITIONS
        "-I${PROJECT_SOURCE_DIR}/naos/freelibcxx/include"
        "-I${PROJECT_SOURCE_DIR}/naos/includes"
    OUTPUT_VARIABLE NAOS_KLOG_COMPILE_FAIL_OUTPUT)
if(NAOS_KLOG_REJECTED)
    message(FATAL_ERROR "KLOG accepted an invalid format/type combination:\n${NAOS_KLOG_COMPILE_FAIL_OUTPUT}")
endif()

if(NOT TARGET Catch2::Catch2WithMain)
    add_subdirectory(
        ${PROJECT_SOURCE_DIR}/naos/third_party/Catch2
        ${CMAKE_BINARY_DIR}/catch2
        EXCLUDE_FROM_ALL)
endif()

include(Catch)

set(NAOS_TEST_INCLUDE_DIRS
    ${CMAKE_BINARY_DIR}/naos
    ${PROJECT_SOURCE_DIR}/naos/freelibcxx/include
    ${PROJECT_SOURCE_DIR}/naos/includes
    ${PROJECT_SOURCE_DIR}/naos/include
    ${PROJECT_SOURCE_DIR}/naos/third_party/libvterm/include
    ${PROJECT_SOURCE_DIR}/naos/src/usr
)

function(add_naos_catch_test target source)
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE ${NAOS_TEST_INCLUDE_DIRS})
    set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    add_dependencies(${target} naos_system_idl)
    catch_discover_tests(${target})
endfunction()

add_naos_catch_test(cfs_vtime_order_test cfs_vtime_order_test.cc)
add_naos_catch_test(phase1_channel_queue_test phase1_channel_queue_test.cc)
add_naos_catch_test(phase0_abi_test phase0_abi_test.cc)
add_naos_catch_test(tls_abi_test tls_abi_test.cc)
add_naos_catch_test(bootstrap_contract_test bootstrap_contract_test.cc)
add_naos_catch_test(capability_restrict_policy_test capability_restrict_policy_test.cc)
add_naos_catch_test(system_idl_test system_idl_test.cc)
add_test(NAME idl_collision_check
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/idl/naoidl.py check-collisions
            ${PROJECT_SOURCE_DIR}/idl/system ${PROJECT_SOURCE_DIR}/idl/internal)

set(NAOS_RUST_IDL_CPP_DIR ${CMAKE_BINARY_DIR}/naos/naos/generated/rust_interop)
set(NAOS_RUST_IDL_CPP_STAMP ${NAOS_RUST_IDL_CPP_DIR}/echo.stamp)
add_custom_command(
    OUTPUT ${NAOS_RUST_IDL_CPP_STAMP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NAOS_RUST_IDL_CPP_DIR}
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/idl/naoidl.py generate
            ${PROJECT_SOURCE_DIR}/doc/examples/echo.naidl ${NAOS_RUST_IDL_CPP_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${NAOS_RUST_IDL_CPP_STAMP}
    DEPENDS ${PROJECT_SOURCE_DIR}/doc/examples/echo.naidl
            ${PROJECT_SOURCE_DIR}/idl/naoidl.py
            ${NAOS_RUST_IDL_CPP_TEMPLATES})
add_custom_target(rust_idl_cpp_binding DEPENDS ${NAOS_RUST_IDL_CPP_STAMP})

set(NAOS_RUST_INTEROP_TARGET_DIR ${CMAKE_BINARY_DIR}/rust-interop-target)
set(NAOS_RUST_INTEROP_BINARY
    ${NAOS_RUST_INTEROP_TARGET_DIR}/x86_64-unknown-linux-gnu/release/rust-interop-fixture)
set(NAOS_RUST_IDL_FIXTURE ${CMAKE_BINARY_DIR}/rust-idl-echo-request.bin)
add_custom_command(
    OUTPUT ${NAOS_RUST_IDL_FIXTURE}
    COMMAND ${NAOS_CARGO_EXECUTABLE} build
            --manifest-path ${PROJECT_SOURCE_DIR}/Cargo.toml
            --locked --release --package rust-interop-fixture
            --target x86_64-unknown-linux-gnu
            --target-dir ${NAOS_RUST_INTEROP_TARGET_DIR}
    COMMAND ${NAOS_RUST_INTEROP_BINARY} encode-request ${NAOS_RUST_IDL_FIXTURE}
    DEPENDS ${PROJECT_SOURCE_DIR}/Cargo.toml ${PROJECT_SOURCE_DIR}/Cargo.lock
            ${PROJECT_SOURCE_DIR}/tests/rust-interop-fixture/Cargo.toml
            ${PROJECT_SOURCE_DIR}/tests/rust-interop-fixture/src/main.rs
            ${PROJECT_SOURCE_DIR}/naos/src/usr/rust/naos-idl/src/lib.rs
            ${PROJECT_SOURCE_DIR}/naos/src/usr/rust/naos-idl/build.rs
            ${PROJECT_SOURCE_DIR}/idl/naoidl.py
            ${PROJECT_SOURCE_DIR}/idl/templates/rust/naoidl/binding.rs.htt
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/naos/src/usr/rust
    VERBATIM)
add_custom_target(rust_idl_fixture DEPENDS ${NAOS_RUST_IDL_FIXTURE})

add_executable(rust_idl_interop_test rust_idl_interop_test.cc)
target_link_libraries(rust_idl_interop_test PRIVATE Catch2::Catch2WithMain)
target_include_directories(rust_idl_interop_test PRIVATE
    ${NAOS_TEST_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}/naos)
target_compile_definitions(rust_idl_interop_test PRIVATE
    NAOS_RUST_IDL_FIXTURE="${NAOS_RUST_IDL_FIXTURE}")
set_target_properties(rust_idl_interop_test PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_dependencies(rust_idl_interop_test rust_idl_cpp_binding rust_idl_fixture)
catch_discover_tests(rust_idl_interop_test)

add_executable(interop_echo_test interop_echo_test.cc)
target_link_libraries(interop_echo_test PRIVATE Catch2::Catch2WithMain)
target_include_directories(interop_echo_test PRIVATE
    ${NAOS_TEST_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}/naos)
target_compile_definitions(interop_echo_test PRIVATE
    NAOS_RUST_INTEROP_BINARY="${NAOS_RUST_INTEROP_BINARY}")
set_target_properties(interop_echo_test PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
add_dependencies(interop_echo_test rust_idl_cpp_binding rust_idl_fixture)
add_test(NAME interop_echo COMMAND interop_echo_test "[interop]")
set_tests_properties(interop_echo PROPERTIES RUN_SERIAL TRUE)

add_naos_catch_test(invocation_deadline_test invocation_deadline_test.cc)
add_naos_catch_test(system_binding_contract_test system_binding_contract_test.cc)
add_naos_catch_test(service_directory_contract_test service_directory_contract_test.cc)
add_naos_catch_test(signal_policy_test signal_policy_test.cc)
add_naos_catch_test(wait_deadline_test wait_deadline_test.cc)
add_naos_catch_test(pvclock_test pvclock_test.cc)
add_naos_catch_test(
    ttyd_terminal_core_test
    ttyd_terminal_core_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/usr/ttyd/terminal_core.cc)
add_naos_catch_test(syscall_header_test syscall_header_test.cc)
add_naos_catch_test(framebuffer_abi_test framebuffer_abi_test.cc)
add_naos_catch_test(input_terminal_shortcut_test input_terminal_shortcut_test.cc)
add_naos_catch_test(log_sink_color_test log_sink_color_test.cc)
add_naos_catch_test(log_panic_format_test log_panic_format_test.cc)
add_naos_catch_test(consoled_input_test consoled_input_test.cc)
add_naos_catch_test(formatter_array_test formatter_array_test.cc)
target_link_libraries(consoled_input_test PRIVATE libvterm_naos)
add_dependencies(consoled_input_test libvterm_naos)

add_test(NAME idl_output_location_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/idl_output_location_test.py
            ${PROJECT_SOURCE_DIR}/idl/system ${CMAKE_BINARY_DIR}/naos/naos/generated/system)

add_test(NAME run_qemu_args_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/run_qemu_args_test.py)
add_test(NAME naos_rust_build_helper_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/naos_rust_build_test.py)
add_test(NAME fstool_disk_smoke
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/fstool_disk_test.py)

# Generate the FAT32 worker fixture under the selected build directory. The
# source tree contains only the generator and textual seed files; tests receive
# the generated path explicitly so a clean checkout does not depend on a
# checked-in binary image.
set(NAOS_FAT32_FIXTURE_DIR ${CMAKE_BINARY_DIR}/exfatd-fixtures)
set(NAOS_FAT32_FIXTURE_IMAGE ${NAOS_FAT32_FIXTURE_DIR}/fat32-fixture.img)
set(NAOS_FAT32_FIXTURE_GZ ${NAOS_FAT32_FIXTURE_IMAGE}.gz)
add_custom_command(
    OUTPUT ${NAOS_FAT32_FIXTURE_IMAGE} ${NAOS_FAT32_FIXTURE_GZ}
    COMMAND ${PROJECT_SOURCE_DIR}/naos/src/usr/rust/exfatd/tests/fixtures/gen_fat32.sh
            ${NAOS_FAT32_FIXTURE_DIR}
    DEPENDS ${PROJECT_SOURCE_DIR}/naos/src/usr/rust/exfatd/tests/fixtures/gen_fat32.sh
            ${PROJECT_SOURCE_DIR}/naos/src/usr/rust/exfatd/tests/fixtures/stage/README.TXT
            ${PROJECT_SOURCE_DIR}/naos/src/usr/rust/exfatd/tests/fixtures/stage/docs/NOTE.TXT
    VERBATIM)
add_custom_target(exfatd_fat32_fixture ALL
    DEPENDS ${NAOS_FAT32_FIXTURE_IMAGE} ${NAOS_FAT32_FIXTURE_GZ})

# Linux-first userspace filesystem acceptance: build and run three independent
# std/Tokio processes (ramdiskd -> exfatd -> vfsd) over local UDS.  This test is
# intentionally outside the QEMU gates and uses the default RAM block backend.
add_test(NAME linux_userspace_filesystem_smoke
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/util/host_services.py
            --cargo ${NAOS_CARGO_EXECUTABLE})
set_tests_properties(linux_userspace_filesystem_smoke PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    TIMEOUT 180
    RUN_SERIAL TRUE)

# Phase 3's FAT worker is host-runnable, while the independent process path is
# covered by boot_smoke_userland_services below. Keep the persistence scenario
# in the normal CTest graph so it cannot silently regress; this test
# intentionally does not claim a QEMU process-boundary or physical-disk run.
find_program(NAOS_HOST_CARGO_EXECUTABLE cargo REQUIRED)
add_test(NAME exfatd_fixture_read
    COMMAND ${NAOS_HOST_CARGO_EXECUTABLE} test --locked -p exfatd --test fixture_read
            --target x86_64-unknown-linux-gnu)
set_tests_properties(exfatd_fixture_read PROPERTIES
    ENVIRONMENT "NAOS_FAT32_FIXTURE=${NAOS_FAT32_FIXTURE_GZ}"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    TIMEOUT 120)

add_test(NAME exfatd_phase3_persistence
    COMMAND ${NAOS_HOST_CARGO_EXECUTABLE} test --locked -p exfatd --test persistence_scenario
            --target x86_64-unknown-linux-gnu)
set_tests_properties(exfatd_phase3_persistence PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    TIMEOUT 120)

# Exercise the userspace MountControl -> Directory -> File data plane through
# the IDL loopback transport.  This is intentionally a host test: the QEMU
# gate below separately proves that the production process boundary starts,
# while this test covers the complete request/response mutation matrix.
add_test(NAME exfatd_application_dataplane
    COMMAND ${NAOS_HOST_CARGO_EXECUTABLE} test --locked -p exfatd
            --features test-loopback --test application_dataplane
            --target x86_64-unknown-linux-gnu)
set_tests_properties(exfatd_application_dataplane PROPERTIES
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    TIMEOUT 120)

add_naos_catch_test(kernel_mobj_exec_test kernel_mobj_exec_test.cc)

# The boot markers below are asserted by the test-owned wrapper, and reaching
# them is what passes this test: the guest has no smoke of its own to run, so
# QEMU stays up until --timeout and the timeout is only a lifecycle bound. The window
# is therefore a boot *deadline*, and it has to absorb a busy host: the guest
# clock dilates when the machine is loaded (parallel builds, other emulators), so
# a window that is merely a large multiple of a quiet boot still flaked at 45s.
# 90s is the value the margins were measured against; shortening it trades gate
# reliability for wall clock, which is the wrong trade for a boot gate.
add_test(NAME boot_smoke_archive
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 90
            --check-boot
            q --iso -n --no-reboot)
set_tests_properties(boot_smoke_archive PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

# The FAT filesystem worker is an independent userland process started by
# vfsd. The default ramdisk is intentionally blank: startup must attempt only
# a mount and report failure, never format the medium implicitly.
add_test(NAME boot_smoke_userland_services
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 90
            --check-boot
            q --iso -n --no-reboot)
set_tests_properties(boot_smoke_userland_services PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

# KVM's negotiated clock and the independent Local APIC event source are
# asserted by the test-owned wrapper.  Keep the generic launcher responsible
# only for image preparation and QEMU lifecycle.
if(EXISTS "/dev/kvm")
    add_test(NAME kvm_pvclock_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 90
                --expect-marker "event-clock=kvm-pvclock"
                --expect-marker "event-source=local-apic"
                --expect-marker "vfsd: INFO: ready service="
                --expect-marker "init: committed root route ready"
                --expect-marker "init: user shell started"
                q --iso -n --no-reboot)
    set_tests_properties(kvm_pvclock_boot PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endif()

# Exercise the mounted userspace FAT data plane only when a prepared medium is
# supplied. The default ramdisk is blank by design; this test must not turn a
# normal boot into an implicit format operation. Enable this test only from a
# build that provides an already-formatted medium.
option(NAOS_VFS_DATA_TEST "Run the FAT data-plane boot test on a prepared medium" OFF)
if(NAOS_VFS_DATA_TEST)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/tests/vfs_data_init.sh")
        message(FATAL_ERROR "NAOS_VFS_DATA_TEST requires tests/vfs_data_init.sh")
    endif()
    add_test(NAME boot_smoke_vfs_data
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 90
                --check-boot
                --expect-marker "vfs-data-smoke: PASS"
                --init-script ${PROJECT_SOURCE_DIR}/tests/vfs_data_init.sh
                q --iso -n --no-reboot)
    set_tests_properties(boot_smoke_vfs_data PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
elseif(EXISTS "${PROJECT_SOURCE_DIR}/tests/vfs_data_init.sh")
    add_test(NAME boot_smoke_vfs_data
        COMMAND ${CMAKE_COMMAND} -E echo
                "Skipping FAT data-plane boot: requires an explicitly prepared medium")
    set_tests_properties(boot_smoke_vfs_data PROPERTIES
        SKIP_REGULAR_EXPRESSION "Skipping FAT data-plane boot"
        TIMEOUT 180
        RUN_SERIAL TRUE)
endif()

# Exercise the MemoryObject shared-page data plane only when the operator asks
# for it.  The default boot path never maps a shared object between processes,
# so this stays opt-in exactly like the FAT data-plane case below.
option(NAOS_MOBJ_SHARE_TEST "Run the MemoryObject shared-page boot test" OFF)
if(NAOS_MOBJ_SHARE_TEST)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/tests/mobj_share_init.sh" OR
       NOT EXISTS "${PROJECT_SOURCE_DIR}/tests/mobj_share_smoke.py")
        message(FATAL_ERROR "NAOS_MOBJ_SHARE_TEST requires the MemoryObject smoke script and wrapper")
    endif()
    add_test(NAME boot_smoke_mobj_share
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/mobj_share_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 90
                q --iso -n --no-reboot)
    set_tests_properties(boot_smoke_mobj_share PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
elseif(EXISTS "${PROJECT_SOURCE_DIR}/tests/mobj_share_init.sh")
    add_test(NAME boot_smoke_mobj_share
        COMMAND ${CMAKE_COMMAND} -E echo
                "Skipping MemoryObject shared-page boot: requires NAOS_MOBJ_SHARE_TEST=ON")
    set_tests_properties(boot_smoke_mobj_share PROPERTIES
        SKIP_REGULAR_EXPRESSION "Skipping MemoryObject shared-page boot"
        TIMEOUT 180
        RUN_SERIAL TRUE)
endif()

# Phase 4 intentionally has no root image path. If a build is assembled
# without the vfsd module, verify that the kernel stops with an explicit
# configuration error instead of silently opening /bin/init through global_root.
# Do not mutate build/bin/system from a test just to manufacture that
# configuration. The helper checks the artifact at test runtime, after the
# build has produced it, so configure-time file existence cannot select a
# stale branch.
add_test(NAME boot_smoke_novfsd
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 90
            --expect-marker "vfsd boot module missing; kernel VFS fallback disabled"
            --require-absent vfsd
            q --iso -n --no-reboot)
set_tests_properties(boot_smoke_novfsd PROPERTIES
    SKIP_REGULAR_EXPRESSION "Skipping no-vfsd negative boot"
    TIMEOUT 180
    RUN_SERIAL TRUE)

add_test(NAME idl_fragment_include_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/idl_fragment_include_test.py
            ${PROJECT_SOURCE_DIR}/idl)
