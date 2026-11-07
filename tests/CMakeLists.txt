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
    if(DEFINED NAOS_CATCH_TEST_TIMEOUT_SECONDS)
        catch_discover_tests(${target} PROPERTIES TIMEOUT ${NAOS_CATCH_TEST_TIMEOUT_SECONDS})
    else()
        catch_discover_tests(${target})
    endif()
endfunction()

add_naos_catch_test(cfs_vtime_order_test cfs_vtime_order_test.cc)
add_naos_catch_test(scheduler_transition_test scheduler_transition_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/schedulers/round_robin.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/schedulers/completely_fair.cc)
target_compile_options(scheduler_transition_test PRIVATE -include cstring)
add_naos_catch_test(thread_lifecycle_test thread_lifecycle_test.cc)
add_naos_catch_test(phase1_channel_queue_test phase1_channel_queue_test.cc)
add_naos_catch_test(phase0_abi_test phase0_abi_test.cc)
add_naos_catch_test(soft_irq_drain_test soft_irq_drain_test.cc)
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
add_naos_catch_test(physical_resource_test physical_resource_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/physical_resource.cc)
add_naos_catch_test(memory_object_dma_policy_test memory_object_dma_policy_test.cc)
add_naos_catch_test(pci_dma_authority_test pci_dma_authority_test.cc)
add_naos_catch_test(pci_dma_mapping_ledger_test pci_dma_mapping_ledger_test.cc)
add_naos_catch_test(pci_dma_domain_test pci_dma_domain_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/pci/dma_domain.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/pci/dma_fault_source.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/physical_resource.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/resource.cc
    ${PROJECT_SOURCE_DIR}/tests/pci_dma_domain_test_support.cc)
add_naos_catch_test(pci_driver_grant_test pci_driver_grant_test.cc)
add_naos_catch_test(pci_osc_authority_test pci_osc_authority_test.cc)
add_naos_catch_test(process_start_lifecycle_test process_start_lifecycle_test.cc)
add_naos_catch_test(irq_vector_pool_test irq_vector_pool_test.cc)
add_naos_catch_test(acpi_execution_thread_binding_test acpi_execution_thread_binding_test.cc)
add_naos_catch_test(tss_io_bitmap_test tss_io_bitmap_test.cc)
add_naos_catch_test(acpi_boot_tables_test acpi_boot_tables_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/arch/acpi/boot_tables.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/intel_vtd_firmware.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/amd_vi_firmware.cc)
add_naos_catch_test(intel_vtd_test intel_vtd_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/arch/acpi/boot_tables.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/intel_vtd_firmware.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/amd_vi_firmware.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/intel_vtd.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/intel_vtd_page_table.cc)
add_naos_catch_test(intel_vtd_interrupt_test intel_vtd_interrupt_test.cc)
add_naos_catch_test(iommu_provider_router_test iommu_provider_router_test.cc)
add_naos_catch_test(amd_vi_dte_test amd_vi_dte_test.cc)
add_naos_catch_test(amd_vi_page_entry_test amd_vi_page_entry_test.cc)
add_naos_catch_test(amd_vi_page_table_test amd_vi_page_table_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/amd_vi_page_table.cc)
add_naos_catch_test(amd_vi_controller_test amd_vi_controller_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/arch/acpi/boot_tables.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/intel_vtd_firmware.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/amd_vi_firmware.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/amd_vi.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/amd_vi_page_table.cc)
set(NAOS_CATCH_TEST_TIMEOUT_SECONDS 10)
add_naos_catch_test(amd_vi_fault_dispatch_test amd_vi_fault_dispatch_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/backend.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/pci/dma_fault_source.cc
    ${PROJECT_SOURCE_DIR}/tests/pci_dma_domain_test_support.cc)
unset(NAOS_CATCH_TEST_TIMEOUT_SECONDS)
add_naos_catch_test(intel_vtd_page_table_test intel_vtd_page_table_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/iommu/intel_vtd_page_table.cc)
add_naos_catch_test(acpipm_counter_test acpipm_counter_test.cc)
add_naos_catch_test(multiboot_boot_module_test multiboot_boot_module_test.cc)
add_naos_catch_test(acpi_fixed_hardware_test acpi_fixed_hardware_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/acpi/fixed_inventory.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/physical_resource.cc)
add_naos_catch_test(vm_fork_policy_test vm_fork_policy_test.cc)
add_naos_catch_test(pci_discovery_scope_test pci_discovery_scope_test.cc)
add_naos_catch_test(pci_topology_lifecycle_test pci_topology_lifecycle_test.cc)
add_naos_catch_test(pci_msi_capability_test pci_msi_capability_test.cc)
add_naos_catch_test(pci_msi_config_test pci_msi_config_test.cc)
add_naos_catch_test(pci_bar_test pci_bar_test.cc)
add_naos_catch_test(memory_object_device_mmio_policy_test memory_object_device_mmio_policy_test.cc)
add_naos_catch_test(pci_dma_quiesce_test pci_dma_quiesce_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/kernel/pci/dma_quiesce.cc)
add_naos_catch_test(io_mapping_allocator_test io_mapping_allocator_test.cc)

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
            --expect-marker "PlatformPowerControl published uri=naos://system/platform/power/control"
            q --iso -m 512 -n --no-reboot)
set_tests_properties(boot_smoke_archive PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

# Exercise read-only ECAM discovery and DMAR inventory on Q35 with a virtual
# Intel IOMMU. Keep this separate from i440fx/pc, which validates no-MCFG boot.
set(NAOS_Q35_PCI_DISCOVERY_EXPECTED_MARKERS)
if(NAOS_PCIED_MSI_ROUTE_SMOKE)
    list(APPEND NAOS_Q35_PCI_DISCOVERY_EXPECTED_MARKERS
        --expect-marker "pcied: INFO: QEMU EDU driver smoke skipped: no compatible test function on this PCI fabric")
endif()
add_test(NAME q35_pci_discovery_boot
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 90
            --expect-marker "PlatformPciTopology published"
            --expect-marker "ACPI PCI _OSC evidence validated; no native control authority root_id="
            --expect-marker "signature=52414d44"
            --expect-marker "ACPI: PCI DMA quiesced functions="
            --expect-marker "ACPI: VT-d default-deny translation active units=1"
            --expect-marker "QI-ready-units=1"
            --expect-marker "AcpiFixedHardware SCI owner claimed"
            --expect-marker "pcied: INFO: PCI root _OSC cached root_id="
            --expect-marker "pcied: INFO: PciFabricAdmin bound"
            --expect-marker "pcied: INFO: pci function inventory built"
            --expect-marker "pcied: INFO: pci ACPI function associations matched=6/6"
            --expect-marker "pcied: INFO: PlatformPciTopology lifecycle watch active"
            --expect-marker "pcied: INFO: pci root bound"
            --expect-marker "pcied: INFO: ready regions=1 roots=1 devices=6"
            ${NAOS_Q35_PCI_DISCOVERY_EXPECTED_MARKERS}
            --no-gdb
            --intel-iommu
            q --machine q35 -m 512 -n)
set_tests_properties(q35_pci_discovery_boot PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

# Exercise VT-d interrupt-remapping setup, fenced global IEC invalidation, and
# MADT/DMAR IOAPIC source identity resolution under QEMU's split IRQ chip.
add_test(NAME q35_pci_interrupt_remapping_prepare_boot
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 45
            --check-boot
            --expect-marker "signature=52414d44"
            --expect-marker "ACPI: VT-d default-deny translation active units=1"
            --expect-marker "QI-ready-units=1"
            --expect-marker "IR-table-ready-units=1"
            --expect-marker "IOAPIC IOMMU source resolved id="
            --expect-marker "IO APIC interrupt remapping active"
            --expect-marker "AcpiFixedHardware SCI owner claimed"
            --expect-marker "pcied: INFO: ready regions=1 roots=1 devices=6"
            --no-gdb
            --intel-iommu-ir
            q --machine q35 -m 512 -n)
set_tests_properties(q35_pci_interrupt_remapping_prepare_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

# Exercise the AMD-Vi ACPI inventory, MMIO admission, default-deny activation,
# and deferred fault poller startup on Q35. AMD interrupt remapping requires a
# split/off kernel irqchip; this host uses KVM's split irqchip mode.
add_test(NAME q35_amd_vi_boot
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 60
            --check-boot
            --expect-marker "ACPI: AMD-Vi register apertures reserved and mapped selected_units=1"
            --expect-marker "ACPI: AMD-Vi default-deny active units=1"
            --expect-marker "ACPI: AMD-Vi DMA provider ready; fault polling active interval_us=1000"
            --expect-marker "PlatformPciTopology published"
            --expect-marker "pcied: INFO: ready regions=1 roots=1 devices="
            --no-gdb
            --qemu-device "amd-iommu,dma-remap=on,intremap=on"
            q --machine q35,kernel-irqchip=split -m 512 -n --no-reboot)
set_tests_properties(q35_amd_vi_boot PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

# Global Lock AML is shared by the PCI smoke suite and the independent software
# fallback gate. Keep its compiler and fixture setup independent of MSI tests.
if(NAOS_PCIED_MSI_ROUTE_SMOKE OR NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK)
    find_program(NAOS_IASL_EXECUTABLE NAMES iasl)
    set(NAOS_QEMU_EDU_ACPI_DIR "${CMAKE_BINARY_DIR}/acpi-test-fixtures")
    set(NAOS_QEMU_GLOBAL_LOCK_ASL "${PROJECT_SOURCE_DIR}/tests/fixtures/qemu_global_lock_ssdt.asl")
    set(NAOS_QEMU_GLOBAL_LOCK_SSDT "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_global_lock_ssdt.aml")
    set(NAOS_IASL_TOOL_DEPENDENCY)

    if(NOT NAOS_IASL_EXECUTABLE)
        find_program(NAOS_MAKE_EXECUTABLE NAMES make REQUIRED)
        set(NAOS_ACPICA_UNIX_DIR "${PROJECT_SOURCE_DIR}/naos/src/acpica/generate/unix")
        set(NAOS_IASL_BUILD_DIR "${CMAKE_BINARY_DIR}/acpica-iasl")
        set(NAOS_IASL_OBJECT_DIR "${NAOS_IASL_BUILD_DIR}/obj")
        set(NAOS_IASL_BINARY_DIR "${NAOS_IASL_BUILD_DIR}/bin")
        set(NAOS_IASL_BUILD_OUTPUT "${NAOS_IASL_BINARY_DIR}/iasl")
        file(RELATIVE_PATH NAOS_IASL_BINDIR
            "${NAOS_ACPICA_UNIX_DIR}"
            "${NAOS_IASL_BINARY_DIR}")
        add_custom_command(
            OUTPUT "${NAOS_IASL_BUILD_OUTPUT}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${NAOS_IASL_OBJECT_DIR}" "${NAOS_IASL_BINARY_DIR}"
            COMMAND "${NAOS_MAKE_EXECUTABLE}"
                    -C "${NAOS_ACPICA_UNIX_DIR}/iasl"
                    "OBJDIR=${NAOS_IASL_OBJECT_DIR}"
                    "BINDIR=${NAOS_IASL_BINDIR}"
                    -j2
            DEPENDS
                "${PROJECT_SOURCE_DIR}/naos/src/acpica/generate/unix/iasl/Makefile"
                "${PROJECT_SOURCE_DIR}/naos/src/acpica/generate/unix/Makefile.config"
            VERBATIM)
        set(NAOS_IASL_EXECUTABLE "${NAOS_IASL_BUILD_OUTPUT}")
        set(NAOS_IASL_TOOL_DEPENDENCY "${NAOS_IASL_BUILD_OUTPUT}")
    endif()

    add_custom_command(
        OUTPUT "${NAOS_QEMU_GLOBAL_LOCK_SSDT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NAOS_QEMU_EDU_ACPI_DIR}"
        COMMAND "${NAOS_IASL_EXECUTABLE}"
                -p "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_global_lock_ssdt"
                "${NAOS_QEMU_GLOBAL_LOCK_ASL}"
        DEPENDS "${NAOS_QEMU_GLOBAL_LOCK_ASL}" ${NAOS_IASL_TOOL_DEPENDENCY}
        VERBATIM)
    add_custom_target(naos_qemu_global_lock_ssdt ALL DEPENDS "${NAOS_QEMU_GLOBAL_LOCK_SSDT}")
    add_dependencies(make-root-image naos_qemu_global_lock_ssdt)
endif()

# This build-only opt-in adds an EDU device and asks pcied to exercise one
# owner-bound interrupt route and DMA domain. It verifies one authorized DMA
# write, a fault on an adjacent unmapped IOVA, both resulting MSIs, and teardown.
if(NAOS_PCIED_MSI_ROUTE_SMOKE)
    set(NAOS_QEMU_EDU_ASL "${PROJECT_SOURCE_DIR}/tests/fixtures/qemu_edu_ssdt.asl")
    set(NAOS_QEMU_EDU_SSDT "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_edu_ssdt.aml")
    set(NAOS_QEMU_EDU_MULTI_ASL "${PROJECT_SOURCE_DIR}/tests/fixtures/qemu_edu_multi_ssdt.asl")
    set(NAOS_QEMU_EDU_MULTI_SSDT "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_edu_multi_ssdt.aml")
    set(NAOS_QEMU_ROCKER_ASL "${PROJECT_SOURCE_DIR}/tests/fixtures/qemu_rocker_ssdt.asl")
    set(NAOS_QEMU_ROCKER_SSDT "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_rocker_ssdt.aml")
    set(NAOS_QEMU_VIRTIO_BLK_ASL "${PROJECT_SOURCE_DIR}/tests/fixtures/qemu_virtio_blk_ssdt.asl")
    set(NAOS_QEMU_VIRTIO_BLK_SSDT "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_virtio_blk_ssdt.aml")
    set(NAOS_QEMU_GPE_ASL "${PROJECT_SOURCE_DIR}/tests/fixtures/qemu_gpe_ssdt.asl")
    set(NAOS_QEMU_GPE_SSDT "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_gpe_ssdt.aml")
    set(NAOS_QEMU_DISABLED_PCI_ROOT_ASL "${PROJECT_SOURCE_DIR}/tests/fixtures/qemu_disabled_pci_root_ssdt.asl")
    set(NAOS_QEMU_DISABLED_PCI_ROOT_SSDT "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_disabled_pci_root_ssdt.aml")

    add_custom_command(
        OUTPUT "${NAOS_QEMU_EDU_SSDT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NAOS_QEMU_EDU_ACPI_DIR}"
        COMMAND "${NAOS_IASL_EXECUTABLE}"
                -p "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_edu_ssdt"
                "${NAOS_QEMU_EDU_ASL}"
        DEPENDS "${NAOS_QEMU_EDU_ASL}" ${NAOS_IASL_TOOL_DEPENDENCY}
        VERBATIM)
    add_custom_target(naos_qemu_edu_ssdt ALL DEPENDS "${NAOS_QEMU_EDU_SSDT}")

    add_custom_command(
        OUTPUT "${NAOS_QEMU_EDU_MULTI_SSDT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NAOS_QEMU_EDU_ACPI_DIR}"
        COMMAND "${NAOS_IASL_EXECUTABLE}"
                -p "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_edu_multi_ssdt"
                "${NAOS_QEMU_EDU_MULTI_ASL}"
        DEPENDS "${NAOS_QEMU_EDU_MULTI_ASL}" ${NAOS_IASL_TOOL_DEPENDENCY}
        VERBATIM)
    add_custom_target(naos_qemu_edu_multi_ssdt ALL DEPENDS "${NAOS_QEMU_EDU_MULTI_SSDT}")

    add_custom_command(
        OUTPUT "${NAOS_QEMU_ROCKER_SSDT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NAOS_QEMU_EDU_ACPI_DIR}"
        COMMAND "${NAOS_IASL_EXECUTABLE}"
                -p "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_rocker_ssdt"
                "${NAOS_QEMU_ROCKER_ASL}"
        DEPENDS "${NAOS_QEMU_ROCKER_ASL}" ${NAOS_IASL_TOOL_DEPENDENCY}
        VERBATIM)
    add_custom_target(naos_qemu_rocker_ssdt ALL DEPENDS "${NAOS_QEMU_ROCKER_SSDT}")

    add_custom_command(
        OUTPUT "${NAOS_QEMU_VIRTIO_BLK_SSDT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NAOS_QEMU_EDU_ACPI_DIR}"
        COMMAND "${NAOS_IASL_EXECUTABLE}"
                -p "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_virtio_blk_ssdt"
                "${NAOS_QEMU_VIRTIO_BLK_ASL}"
        DEPENDS "${NAOS_QEMU_VIRTIO_BLK_ASL}" ${NAOS_IASL_TOOL_DEPENDENCY}
        VERBATIM)
    add_custom_target(naos_qemu_virtio_blk_ssdt ALL DEPENDS "${NAOS_QEMU_VIRTIO_BLK_SSDT}")

    add_custom_command(
        OUTPUT "${NAOS_QEMU_GPE_SSDT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NAOS_QEMU_EDU_ACPI_DIR}"
        COMMAND "${NAOS_IASL_EXECUTABLE}"
                -p "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_gpe_ssdt"
                "${NAOS_QEMU_GPE_ASL}"
        DEPENDS "${NAOS_QEMU_GPE_ASL}" ${NAOS_IASL_TOOL_DEPENDENCY}
        VERBATIM)
    add_custom_target(naos_qemu_gpe_ssdt ALL DEPENDS "${NAOS_QEMU_GPE_SSDT}")

    add_custom_command(
        OUTPUT "${NAOS_QEMU_DISABLED_PCI_ROOT_SSDT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NAOS_QEMU_EDU_ACPI_DIR}"
        COMMAND "${NAOS_IASL_EXECUTABLE}"
                -p "${NAOS_QEMU_EDU_ACPI_DIR}/qemu_disabled_pci_root_ssdt"
                "${NAOS_QEMU_DISABLED_PCI_ROOT_ASL}"
        DEPENDS "${NAOS_QEMU_DISABLED_PCI_ROOT_ASL}" ${NAOS_IASL_TOOL_DEPENDENCY}
        VERBATIM)
    add_custom_target(naos_qemu_disabled_pci_root_ssdt ALL DEPENDS "${NAOS_QEMU_DISABLED_PCI_ROOT_SSDT}")

    # The test SSDT replaces QEMU's _E02 body with NoOp so the actual CPU
    # hotplug GPE.2 reaches SCI acknowledgement without opting into QEMU's
    # unsupported CPU hotplug Notify policy.
    add_test(NAME q35_acpi_sci_gpe_hotplug_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/acpi_gpe_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 60
                --acpi-ssdt "${NAOS_QEMU_GPE_SSDT}")
    set_tests_properties(q35_acpi_sci_gpe_hotplug_boot PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

    # Exercise the kernel's FACS/SCI Global Lock path through a _GLK device.
    # The _INI has a nested explicit Acquire/Release, while snapshot discovery
    # must still complete _STA and retain the device as healthy.
    add_test(NAME q35_acpi_global_lock_hid_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 30
                --check-boot
                --acpi-ssdt "${NAOS_QEMU_GLOBAL_LOCK_SSDT}"
                --expect-marker "ACPI: FACS Global Lock SCI event enabled"
                --expect-marker "platformd: INFO: AML namespace uses the host-managed ACPI Global Lock protocol"
                --expect-marker "platformd: INFO: ACPI device path=\\_SB_.GLKT status=f initialized=true degraded=false"
                --expect-marker "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Hid value=String(\"NAOS0001\")"
                --expect-marker "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Cid value=String(\"ACME0001\")"
                --expect-marker "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Uid value=Integer(42)"
                --expect-marker "pcied: INFO: PlatformPciTopology ACPI PCI device paths snapshots="
                --expect-marker-order
                "ACPI: FACS Global Lock SCI event enabled"
                "platformd: INFO: ACPI device path=\\_SB_.GLKT status=f initialized=true degraded=false"
                "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Hid value=String(\"NAOS0001\")"
                "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Cid value=String(\"ACME0001\")"
                "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Uid value=Integer(42)"
                "platformd: INFO: AML namespace uses the host-managed ACPI Global Lock protocol"
                --no-gdb
                q --machine q35 -m 512 -n --no-reboot)
    set_tests_properties(q35_acpi_global_lock_hid_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

    # A root bridge with a valid PCI _HID/_CRS but _STA=0 must not be
    # published to pcied or included in its root scan.
    add_test(NAME q35_acpi_disabled_pci_root_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 45
                --check-boot
                --acpi-ssdt "${NAOS_QEMU_DISABLED_PCI_ROOT_SSDT}"
                --expect-marker "ACPI PCI roots omitted because _STA did not confirm present and functioning count=1"
                --expect-marker "platformd: INFO: ACPI device path=\\_SB_.OFF0 status=0"
                --expect-marker "pcied: INFO: ready regions=1 roots=1 devices=6"
                --no-gdb
                --intel-iommu
                q --machine q35 -m 512 -n)
    set_tests_properties(q35_acpi_disabled_pci_root_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

    if(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE AND NOT NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT AND
       NOT NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
        # This positive waiter test needs the QEMU ICH9 PM1 Global Lock event
        # fixture in tests/fixtures/qemu-11.1.1-acpi-global-lock-sci.patch.
        # The guest writes its test hook; QEMU sets PM1 GBL_STS and raises the
        # ordinary SCI IRQ consumed by the kernel.
        add_test(NAME q35_acpi_global_lock_waiter_wake_boot
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                    --build-dir ${CMAKE_BINARY_DIR}
                    --timeout 30
                    --check-boot
                    --acpi-ssdt "${NAOS_QEMU_GLOBAL_LOCK_SSDT}"
                    --qemu-trace-event "ich9_acpi_global_lock_test_gbl_sts"
                    --expect-marker "ACPI TEST: firmware actor acquired FACS Global Lock"
                    --expect-marker "platformd: INFO: ACPI TEST: kernel Global Lock finite-timeout outcome verified"
                    --expect-marker "ACPI TEST: QEMU PM1 GBL_STS event requested for blocked waiter"
                    --expect-marker "ACPI TEST: QEMU PM1 GBL_STS event requested for claimed SCI owner"
                    --expect-marker-count "ACPI: PM1 Global Lock status observed on SCI" 2
                    --expect-marker "platformd: INFO: PM1 Global Lock status acknowledged after kernel wake"
                    --expect-marker "platformd: INFO: ACPI device path=\\_SB_.GLKT status=f initialized=true degraded=false"
                    --expect-qemu-trace-regex "ich9_acpi_global_lock_test_gbl_sts"
                    --no-gdb
                    q --machine q35 -m 512 -n --no-reboot)
        set_tests_properties(q35_acpi_global_lock_waiter_wake_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()

    if(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT)
        add_test(NAME q35_acpi_global_lock_owner_exit_boot
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                    --build-dir ${CMAKE_BINARY_DIR}
                    --timeout 45
                    --acpi-ssdt "${NAOS_QEMU_GLOBAL_LOCK_SSDT}"
                    --expect-marker "ACPI TEST: platformd acquired FACS Global Lock before process owner exit"
                    --expect-marker "ACPI TEST: kernel released FACS Global Lock during process owner exit"
                    --expect-marker-order
                    "ACPI TEST: platformd acquired FACS Global Lock before process owner exit"
                    "ACPI TEST: kernel released FACS Global Lock during process owner exit"
                    --no-gdb
                    q --machine q35 -m 512 -n --no-reboot)
        set_tests_properties(q35_acpi_global_lock_owner_exit_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()

    if(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
        add_test(NAME q35_acpi_global_lock_release_failure_boot
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                    --build-dir ${CMAKE_BINARY_DIR}
                    --timeout 15
                    --acpi-ssdt "${NAOS_QEMU_GLOBAL_LOCK_SSDT}"
                    --expect-marker "ACPI TEST: platformd acquired FACS Global Lock before injected release failure"
                    --expect-marker "ACPI TEST: FACS Global Lock release-failure path quarantined"
                    --expect-marker "ACPI TEST: kernel returned GlobalLockIoError after injected release failure"
                    --expect-marker "ACPI TEST: Global Lock stayed fail-closed after release failure"
                    --expect-marker-order
                    "ACPI TEST: platformd acquired FACS Global Lock before injected release failure"
                    "ACPI TEST: FACS Global Lock release-failure path quarantined"
                    "ACPI TEST: kernel returned GlobalLockIoError after injected release failure"
                    "ACPI TEST: Global Lock stayed fail-closed after release failure"
                    --no-gdb
                    q --machine q35 -m 512 -n --no-reboot)
        set_tests_properties(q35_acpi_global_lock_release_failure_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    endif()

    # Intel VT-d delivers the read-only mapping fault on stock and patched
    # QEMU. Keep its expectation independent from the AMD-only IOPF option;
    # only the second, post-drain fault address changes with that option.
    set(NAOS_INTEL_EDU_FAULT_EXPECTATIONS
        --expect-marker "edud: INFO: QEMU EDU DMA read-only IOVA write rejected iova=00005000"
        --expect-qemu-trace-regex "vtd_dmar_fault sid 0x18 fault [0-9]+ addr 0x5000 write 1")
    if(NAOS_TEST_AMDVI_MEMORY_IOPF)
        list(APPEND NAOS_INTEL_EDU_FAULT_EXPECTATIONS
            --expect-marker "edud: INFO: QEMU EDU IOMMU fault queued before source close iova=00005000")
    else()
        list(APPEND NAOS_INTEL_EDU_FAULT_EXPECTATIONS
            --expect-marker "edud: INFO: QEMU EDU IOMMU fault queued before source close iova=00006000")
    endif()

    add_test(NAME q35_pci_msi_route_prepare_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 45
                --check-boot
                --acpi-ssdt "${NAOS_QEMU_EDU_SSDT}"
                --qemu-trace-event "vtd_ir_remap_msi_req"
                --qemu-trace-event "vtd_ir_irte_get"
                --qemu-trace-event "vtd_ir_remap"
                --qemu-trace-event "vtd_ir_remap_type"
                --qemu-trace-event "vtd_frr_new"
                --qemu-trace-event "vtd_inv_desc"
                --qemu-trace-event "vtd_inv_desc_cc_global"
                --qemu-trace-event "vtd_inv_desc_iotlb_global"
                --qemu-trace-event "vtd_inv_desc_wait_sw"
                --qemu-trace-event "vtd_dmar_fault"
                --qemu-trace-event "pci_cfg_write"
                --expect-marker "ACPI device path=\\_SB_.PCI0.S18_ status=f initialized=true degraded=false resources=1"
                --expect-marker "PCI device resource attestation bound address=0000:00:03.0"
                --expect-marker "pcied: INFO: scoped PCI ServiceDirectory view resolved PciDiscoveryFabric"
                --expect-marker "pcied: INFO: PCI driver manager preflight complete matches=1 functions=7"
                --expect-marker "pcied: INFO: PCI driver match address=0000:00:03.0 compatible_id=QEMU0002 image=edud"
                --expect-marker "pcied: INFO: single-URI PCI ServiceDirectory view rejected nested view creation"
                --expect-marker "pcied: INFO: single-URI PCI ServiceDirectory view denied unrelated locator"
                --expect-marker "edud: INFO: QEMU EDU BAR0 mapped id=010000ed"
                --expect-marker "edud: INFO: QEMU EDU PCI config read verified vendor=1234 device=11e8"
                --expect-marker "pcied: INFO: PCI driver grants staged before child start count=1"
                --expect-marker "edud: INFO: child ServiceDirectory cannot resolve driver image locator"
                --expect-marker "edud: INFO: QEMU EDU child owns DMA/IRQ; BAR1 denied address=0000:00:03.0"
                --expect-marker "edud: INFO: QEMU EDU child published private data resource address=0000:00:03.0"
                --expect-marker "pcied: INFO: PCI driver child data resource stayed private address=0000:00:03.0"
                --expect-marker "pcied: INFO: QEMU EDU granted driver process exited address=0000:00:03.0"
                --expect-marker "edud: INFO: QEMU EDU default-deny DMA domain ready bus_master=enabled mappings=1"
                --expect-marker "edud: INFO: QEMU EDU DMA transfer verified bytes=64"
                ${NAOS_INTEL_EDU_FAULT_EXPECTATIONS}
                --expect-marker "edud: INFO: QEMU EDU IOMMU fault delivered bus_master=enabled sequence="
                --expect-marker "edud: INFO: QEMU EDU IOMMU fault drained after source close sequence="
                --expect-marker "edud: INFO: QEMU EDU MSI delivered sequence="
                --expect-marker "edud: INFO: QEMU EDU MSI device ack completed"
                --expect-marker "edud: INFO: QEMU EDU MSI route source closed"
                --expect-marker "edud: INFO: QEMU EDU DMA domain bus_master=disabled"
                --expect-marker "edud: INFO: QEMU EDU DMA page unmapped"
                --expect-marker "edud: INFO: QEMU EDU DMA domain closed"
                --expect-marker "PCI MSI route teardown complete"
                --no-gdb
                --intel-iommu-ir
                --qemu-device edu
                q --machine q35 -m 512 -n)
    set_tests_properties(q35_pci_msi_route_prepare_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

    # Exercise AMD-Vi DMA authorization, MSI remapping, a denied DMA trace, and
    # owner teardown. Stock QEMU 11.1.1 marks memory page-walk faults as
    # interrupt faults and drops the second event qword. The opt-in positive
    # branch requires QEMU built with
    # tests/fixtures/qemu-11.1.1-amdvi-memory-iopf.patch.
    set(NAOS_AMDVI_DMA_SMOKE_OUTCOME_MARKERS)
    if(NAOS_TEST_AMDVI_MEMORY_IOPF)
        list(APPEND NAOS_AMDVI_DMA_SMOKE_OUTCOME_MARKERS
            --expect-marker "edud: INFO: QEMU EDU DMA read-only IOVA write rejected iova=00005000"
            --expect-marker "edud: INFO: QEMU EDU IOMMU fault delivered bus_master=enabled"
            --expect-marker "edud: INFO: QEMU EDU IOMMU fault queued before source close iova=00005000"
            --expect-marker "edud: INFO: QEMU EDU IOMMU fault drained after source close"
            --expect-marker "edud: INFO: QEMU EDU DMA domain closed"
            --expect-marker "edud: INFO: QEMU EDU MSI route source closed address=0000:00:03.0"
            --expect-marker "pcied: INFO: QEMU EDU granted driver process exited address=0000:00:03.0 pid="
            --expect-qemu-trace-regex "pci_cfg_write edu 00:03\\.0 @0x4 <- 0x103"
            --forbid-marker "edud: WARN: QEMU EDU DMA fault not reported (")
    else()
        list(APPEND NAOS_AMDVI_DMA_SMOKE_OUTCOME_MARKERS
            --expect-marker "edud: INFO: QEMU EDU DMA read-only IOVA write rejected iova=00005000"
            --expect-marker "edud: WARN: QEMU EDU DMA fault not reported ("
            --expect-marker "edud: INFO: QEMU EDU DMA domain bus_master=disabled"
            --expect-marker "edud: INFO: QEMU EDU DMA domain closed after missing fault report"
            --expect-marker "edud: INFO: QEMU EDU MSI route source closed after missing fault report"
            --expect-marker "pcied: ERROR: PCI driver child outcome failed: granted PCI driver 0000:00:03.0 pid=")
    endif()
    add_test(NAME q35_pci_amd_vi_dma_msi_fault_trace_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 90
                --check-boot
                --acpi-ssdt "${NAOS_QEMU_EDU_SSDT}"
                --qemu-trace-event "amdvi_page_fault"
                --qemu-trace-event "amdvi_ir_remap_msi"
                --qemu-trace-event "amdvi_iotlb_inval"
                --qemu-trace-event "amdvi_devtab_inval"
                --qemu-trace-event "pci_cfg_write"
                --expect-marker "ACPI: AMD-Vi default-deny active units=1"
                --expect-marker "ACPI: AMD-Vi DMA provider ready; fault polling active interval_us=1000"
                --expect-marker "AMD-Vi event log drained unit=0 records="
                --expect-marker "ACPI device path=\\_SB_.PCI0.S18_ status=f initialized=true degraded=false resources=1"
                --expect-marker "PCI device resource attestation bound address=0000:00:03.0"
                --expect-marker "pcied: INFO: PCI driver manager preflight complete matches=1 functions="
                --expect-marker "pcied: INFO: PCI driver match address=0000:00:03.0 compatible_id=QEMU0002 image=edud"
                --expect-marker "edud: INFO: QEMU EDU default-deny DMA domain ready bus_master=enabled mappings=1"
                --expect-marker "edud: INFO: QEMU EDU DMA transfer verified bytes=64"
                --expect-marker "edud: INFO: QEMU EDU MSI delivered sequence="
                ${NAOS_AMDVI_DMA_SMOKE_OUTCOME_MARKERS}
                --expect-marker "PCI MSI route teardown complete"
                --forbid-marker "PCI MSI route teardown quarantined"
                --expect-qemu-trace-regex "amdvi_page_fault"
                --expect-qemu-trace-regex "amdvi_ir_remap_msi"
                --no-gdb
                --qemu-device "AMDVI-PCI,id=amdvi,bus=pcie.0,addr=0x1e"
                --qemu-device "amd-iommu,pci-id=amdvi,dma-remap=on,intremap=on"
                --qemu-device "edu"
                q --machine q35,kernel-irqchip=split -m 512 -n --no-reboot)
    set_tests_properties(q35_pci_amd_vi_dma_msi_fault_trace_boot PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

    # Exercise one immutable match plan for two compatible PCI functions,
    # stage both grants before starting either isolated child, then collect
    # each child's exit against its own _CRS BAR.
    add_test(NAME q35_pci_multi_driver_grant_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 30
                --check-boot
                --acpi-ssdt "${NAOS_QEMU_EDU_MULTI_SSDT}"
                --qemu-trace-event "vtd_dmar_fault"
                --expect-marker "PCI device resource attestation bound address=0000:00:03.0"
                --expect-marker "PCI device resource attestation bound address=0000:00:04.0"
                --expect-marker "pcied: INFO: PCI driver manager preflight complete matches=2 functions=8"
                --expect-marker-count "PCI driver match address=" 2
                --expect-marker "pcied: INFO: PCI driver grants staged before child start count=2"
                --expect-marker-count "pcied: INFO: PCI child Process.start retry confirmed idempotent address=" 2
                --expect-marker "pcied: INFO: PCI driver start phase complete started=2 awaiting child exits"
                --expect-marker-count "edud: INFO: QEMU EDU child published private data resource address=" 2
                --expect-marker-count "pcied: INFO: PCI driver child data resource stayed private address=" 2
                --expect-marker-order
                "pcied: INFO: PCI driver manager preflight complete matches=2 functions=8"
                "pcied: INFO: PCI driver grants staged before child start count=2"
                "pcied: INFO: PCI driver start phase complete started=2 awaiting child exits"
                "pcied: INFO: QEMU EDU granted driver process exited address="
                --expect-marker-count "QEMU EDU granted driver process exited address=" 2
                --expect-marker-count "edud: INFO: QEMU EDU MSI device ack completed sequence=1" 2
                --expect-marker-count "PCI MSI route teardown complete" 2
                --expect-qemu-trace-regex "vtd_dmar_fault sid 0x18 fault [0-9]+ addr 0x5000 write 1"
                --expect-qemu-trace-regex "vtd_dmar_fault sid 0x20 fault [0-9]+ addr 0x5000 write 1"
                --no-gdb
                --intel-iommu-ir
                --qemu-device edu
                --qemu-device edu
                q --machine q35 -m 512 -n)
    set_tests_properties(q35_pci_multi_driver_grant_boot PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

    # The QEMU Rocker test device supports only MSI-X. Its fixed BAR _CRS
    # fixture lets the kernel own the MSI-X table mapping and validate a real
    # MSI-X delivery without exposing the table BAR as generic MMIO.
    add_test(NAME q35_pci_msix_route_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 30
                --check-boot
                --acpi-ssdt "${NAOS_QEMU_ROCKER_SSDT}"
                --qemu-trace-event "vtd_ir_remap_msi_req"
                --qemu-trace-event "vtd_ir_irte_get"
                --qemu-trace-event "vtd_ir_remap"
                --expect-marker "ACPI device path=\\_SB_.PCI0.S18_ status=f initialized=true degraded=false resources=2"
                --expect-marker "PCI device resource attestation bound address=0000:00:03.0"
                --expect-marker "pcied: INFO: QEMU Rocker MSI-X capability verified address=0000:00:03.0 vectors="
                --expect-marker "pcied: INFO: QEMU Rocker BAR0 liveness verified bytes=8192 value=2468acf0"
                --expect-marker "pcied: INFO: QEMU Rocker MSI-X route prepared address=0000:00:03.0"
                --expect-marker "pcied: INFO: QEMU Rocker MSI-X test ready bus_master=enabled mappings=0"
                --expect-marker "pcied: INFO: QEMU Rocker MSI-X source bound address=0000:00:03.0"
                --expect-marker "pcied: INFO: QEMU Rocker TEST_IRQ requested MSI-X vector=0"
                --expect-marker "pcied: INFO: QEMU Rocker MSI-X interrupt delivered sequence=1"
                --expect-marker "pcied: INFO: QEMU Rocker MSI-X event acknowledged sequence=1"
                --expect-marker "pcied: INFO: QEMU Rocker MSI-X test domain closed bus_master=disabled"
                --expect-marker "pcied: INFO: QEMU Rocker MSI-X route source closed address=0000:00:03.0"
                --expect-marker "PCI MSI route teardown complete segment=0 bus=0 device=3 function=0"
                --expect-qemu-trace-regex "vtd_ir_remap_msi_req"
                --expect-qemu-trace-regex "vtd_ir_remap_msi_req addr 0xfee000[0-9a-f]+ data 0x0"
                --no-gdb
                --intel-iommu-ir
                --qemu-device rocker
                q --machine q35 -m 512 -n)
    set_tests_properties(q35_pci_msix_route_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

    if(NOT NAOS_PCIED_VIRTIO_BLOCK_FAULT_SMOKE)
        # Exercise the production modern Virtio Block child through its kernel-
        # routed MSI-X vector, isolated DMA domain, catalog lease, and block I/O.
        add_test(NAME q35_virtio_block_msix_io_boot
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/virtio_block_boot_smoke.py
                    --build-dir ${CMAKE_BINARY_DIR}
                    --timeout 75)
        set_tests_properties(q35_virtio_block_msix_io_boot PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
    endif()

    if(NAOS_PCIED_VIRTIO_BLOCK_FAULT_SMOKE)
        # Inject a terminal unmapped DMA read into the real child driver and
        # verify RPC failure, provider removal, and kernel resource teardown.
        add_test(NAME q35_virtio_block_terminal_dma_fault_boot
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/virtio_block_boot_smoke.py
                    --build-dir ${CMAKE_BINARY_DIR}
                    --timeout 75
                    --terminal-fault)
        set_tests_properties(q35_virtio_block_terminal_dma_fault_boot PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
    endif()

    # The documented make-root-image build precedes boot tests. Keep the
    # generated PCI smoke SSDTs current there too.
add_dependencies(make-root-image naos_qemu_edu_ssdt naos_qemu_edu_multi_ssdt naos_qemu_rocker_ssdt
                 naos_qemu_virtio_blk_ssdt naos_qemu_gpe_ssdt naos_qemu_disabled_pci_root_ssdt)
endif()

if(NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK)
    # Exercise the local Global Lock owner/wait-queue path independently of the
    # PCI/MSI smoke suite. The fixture's _INI performs nested Acquire/Release.
    add_test(NAME q35_acpi_global_lock_software_fallback_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 30
                --check-boot
                --acpi-ssdt "${NAOS_QEMU_GLOBAL_LOCK_SSDT}"
                --expect-marker "ACPI: FACS Global Lock SCI event enabled"
                --expect-marker "ACPI TEST: forcing local Global Lock software serialization"
                --expect-marker "platformd: INFO: AML namespace uses the host-managed ACPI Global Lock protocol"
                --expect-marker "platformd: INFO: ACPI device path=\\_SB_.GLKT status=f initialized=true degraded=false"
                --expect-marker "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Hid value=String(\"NAOS0001\")"
                --expect-marker "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Cid value=String(\"ACME0001\")"
                --expect-marker "platformd: INFO: ACPI device identifier path=\\_SB_.GLKT kind=Uid value=Integer(42)"
                --expect-marker "pcied: INFO: PlatformPciTopology ACPI PCI device paths snapshots="
                --no-gdb
                q --machine q35 -m 512 -n --no-reboot)
    set_tests_properties(q35_acpi_global_lock_software_fallback_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()

# A host bridge may expose MCFG without an IOMMU. Firmware-enabled bus mastering
# must still be quiesced before the read-only discovery service is published.
add_test(NAME q35_pci_no_iommu_dma_quiesce_boot
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 45
            --forbid-marker "signature=52414d44"
            --expect-marker "ACPI: PCI DMA quiesced functions="
            --expect-marker "pcied: INFO: ready regions=1 roots=1 devices=6"
            --no-gdb
            q --machine q35 -m 512 -n)
set_tests_properties(q35_pci_no_iommu_dma_quiesce_boot PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)

if(NAOS_TEST_ACPI_FACS_RESERVATION_COLLISION)
    add_test(NAME acpi_facs_reservation_collision_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 25
                --expect-marker "ACPI TEST FACS reservation collision injected"
                --expect-marker "ACPI: unable to ledger FACS firmware state"
                --expect-marker "FirmwareRoot service unavailable; ACPI snapshot was not built"
                --forbid-marker "FirmwareRoot service published"
                --forbid-marker "AcpiResourceAuthority service published"
                --forbid-marker "PciDiscoveryFabric service published"
                --forbid-marker "AcpiFixedHardware service published"
                --forbid-marker "PANIC:"
                --forbid-marker "Kernel Oops"
                --forbid-marker "exception "
                --no-gdb
                q --machine q35 -m 512 -n)
    set_tests_properties(acpi_facs_reservation_collision_boot PROPERTIES TIMEOUT 75 RUN_SERIAL TRUE)
endif()

if(NAOS_TEST_ACPI_INIT_CAPABILITY_ROLLBACK)
    add_test(NAME acpi_init_capability_rollback_boot
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
                --build-dir ${CMAKE_BINARY_DIR}
                --timeout 25
                --check-boot
                --expect-marker "ACPI TEST: capability publication rollback injected"
                --expect-marker "FirmwareRoot service unavailable; ACPI snapshot was not built"
                --forbid-marker "FirmwareRoot service published"
                --forbid-marker "PciDiscoveryFabric service published"
                --forbid-marker "AcpiFixedHardware service published"
                --forbid-marker "AcpiResourceAuthority service published"
                --forbid-marker "PANIC:"
                --forbid-marker "Kernel Oops"
                --forbid-marker "exception "
                --no-gdb
                q --machine q35 -m 512 -n)
    set_tests_properties(acpi_init_capability_rollback_boot PROPERTIES TIMEOUT 75 RUN_SERIAL TRUE)
endif()

# The FAT filesystem worker is an independent userland process started by
# vfsd. The default ramdisk is intentionally blank: startup must attempt only
# a mount and report failure, never format the medium implicitly.
add_test(NAME boot_smoke_userland_services
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 90
            --check-boot
            q --iso -m 512 -n --no-reboot)
set_tests_properties(boot_smoke_userland_services PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

# This opt-in init hook asks platformd for a typed reset after the normal
# services have published. QEMU's -no-reboot turns the guest reset into a
# clean emulator exit, so the test cannot loop through the hook on a reboot.
# The full debug userland exhausted a 128 MiB guest during this boot path.
add_test(NAME boot_smoke_platform_power_reset
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/boot_smoke.py
            --build-dir ${CMAKE_BINARY_DIR}
            --timeout 90
            --no-gdb
            --init-script ${PROJECT_SOURCE_DIR}/tests/platform_power_reset_init.sh
            --expect-qemu-exit
            --expect-marker "PlatformPowerControl published uri=naos://system/platform/power/control"
            --expect-marker "reboot: submitting PlatformPowerControl reboot request"
            q --machine q35 --iso -n --no-reboot -m 512)
set_tests_properties(boot_smoke_platform_power_reset PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

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
                q --iso -m 512 -n --no-reboot)
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
                q --iso -m 512 -n --no-reboot)
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
                q --iso -m 512 -n --no-reboot)
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
            q --iso -m 512 -n --no-reboot)
set_tests_properties(boot_smoke_novfsd PROPERTIES
    SKIP_REGULAR_EXPRESSION "Skipping no-vfsd negative boot"
    TIMEOUT 180
    RUN_SERIAL TRUE)

add_test(NAME idl_fragment_include_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/idl_fragment_include_test.py
            ${PROJECT_SOURCE_DIR}/idl)
