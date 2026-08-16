find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
add_naos_catch_test(input_terminal_shortcut_test input_terminal_shortcut_test.cc)
add_naos_catch_test(log_sink_color_test log_sink_color_test.cc)
add_naos_catch_test(log_panic_format_test log_panic_format_test.cc)
add_naos_catch_test(consoled_input_test consoled_input_test.cc)
add_naos_catch_test(consoled_render_batch_test consoled_render_batch_test.cc)
add_naos_catch_test(formatter_array_test formatter_array_test.cc)
target_link_libraries(consoled_input_test PRIVATE libvterm_naos)
add_dependencies(consoled_input_test libvterm_naos)

add_test(NAME idl_output_location_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/idl_output_location_test.py
            ${PROJECT_SOURCE_DIR}/idl/system ${CMAKE_BINARY_DIR}/naos/naos/generated/system)

add_test(NAME run_qemu_args_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/run_qemu_args_test.py)
