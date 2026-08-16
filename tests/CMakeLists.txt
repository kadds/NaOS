find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(NAOS_UNIT_TEST_SOURCES
    unit_tests_main.cc
    cfs_vtime_order_test.cc
    phase1_channel_queue_test.cc
    phase0_abi_test.cc
    bootstrap_contract_test.cc
    capability_restrict_policy_test.cc
    system_idl_test.cc
    invocation_deadline_test.cc
    system_binding_contract_test.cc
    service_directory_contract_test.cc
    signal_policy_test.cc
    wait_deadline_test.cc
    ttyd_terminal_core_test.cc
    syscall_header_test.cc
    framebuffer_abi_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/usr/ttyd/terminal_core.cc
)

add_executable(naos_unit_tests ${NAOS_UNIT_TEST_SOURCES})
target_include_directories(naos_unit_tests PRIVATE
    ${CMAKE_BINARY_DIR}/naos
    ${PROJECT_SOURCE_DIR}/naos/freelibcxx/include
    ${PROJECT_SOURCE_DIR}/naos/includes
    ${PROJECT_SOURCE_DIR}/naos/include
    ${PROJECT_SOURCE_DIR}/naos/src/usr
)
set_target_properties(naos_unit_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_dependencies(naos_unit_tests naos_system_idl)
add_test(NAME naos_unit_tests COMMAND naos_unit_tests)

add_test(NAME idl_output_location_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/idl_output_location_test.py
            ${PROJECT_SOURCE_DIR}/idl/system ${CMAKE_BINARY_DIR}/naos/naos/generated/system)
