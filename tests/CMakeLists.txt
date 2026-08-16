find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
    ${PROJECT_SOURCE_DIR}/naos/src/usr
)

function(add_naos_catch_test target source)
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    target_include_directories(${target} PRIVATE ${NAOS_TEST_INCLUDE_DIRS})
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_dependencies(${target} naos_system_idl)
    catch_discover_tests(${target})
endfunction()

add_naos_catch_test(cfs_vtime_order_test cfs_vtime_order_test.cc)
add_naos_catch_test(phase1_channel_queue_test phase1_channel_queue_test.cc)
add_naos_catch_test(phase0_abi_test phase0_abi_test.cc)
add_naos_catch_test(bootstrap_contract_test bootstrap_contract_test.cc)
add_naos_catch_test(capability_restrict_policy_test capability_restrict_policy_test.cc)
add_naos_catch_test(system_idl_test system_idl_test.cc)
add_naos_catch_test(invocation_deadline_test invocation_deadline_test.cc)
add_naos_catch_test(system_binding_contract_test system_binding_contract_test.cc)
add_naos_catch_test(service_directory_contract_test service_directory_contract_test.cc)
add_naos_catch_test(signal_policy_test signal_policy_test.cc)
add_naos_catch_test(wait_deadline_test wait_deadline_test.cc)
add_naos_catch_test(
    ttyd_terminal_core_test
    ttyd_terminal_core_test.cc
    ${PROJECT_SOURCE_DIR}/naos/src/usr/ttyd/terminal_core.cc)
add_naos_catch_test(syscall_header_test syscall_header_test.cc)
add_naos_catch_test(framebuffer_abi_test framebuffer_abi_test.cc)

add_test(NAME idl_output_location_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/idl_output_location_test.py
            ${PROJECT_SOURCE_DIR}/idl/system ${CMAKE_BINARY_DIR}/naos/naos/generated/system)
