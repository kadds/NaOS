find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(phase1_channel_queue_test phase1_channel_queue_test.cc)
target_include_directories(phase1_channel_queue_test PRIVATE
    ${PROJECT_SOURCE_DIR}/naos/includes
    ${PROJECT_SOURCE_DIR}/naos/include
)
set_target_properties(phase1_channel_queue_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME phase1_channel_queue_test COMMAND phase1_channel_queue_test)

add_executable(phase0_abi_test phase0_abi_test.cc)
target_include_directories(phase0_abi_test PRIVATE
    ${PROJECT_SOURCE_DIR}/naos/include
)
set_target_properties(phase0_abi_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME phase0_abi_test COMMAND phase0_abi_test)

add_executable(bootstrap_contract_test bootstrap_contract_test.cc)
target_include_directories(bootstrap_contract_test PRIVATE
    ${PROJECT_SOURCE_DIR}/naos/include
)
set_target_properties(bootstrap_contract_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME bootstrap_contract_test COMMAND bootstrap_contract_test)

add_executable(capability_restrict_policy_test capability_restrict_policy_test.cc)
target_include_directories(capability_restrict_policy_test PRIVATE
    ${PROJECT_SOURCE_DIR}/naos/freelibcxx/include
    ${PROJECT_SOURCE_DIR}/naos/includes
    ${PROJECT_SOURCE_DIR}/naos/include
)
set_target_properties(capability_restrict_policy_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME capability_restrict_policy_test COMMAND capability_restrict_policy_test)

add_executable(system_idl_test system_idl_test.cc)
target_include_directories(system_idl_test PRIVATE
    ${CMAKE_BINARY_DIR}/naos
    ${PROJECT_SOURCE_DIR}/naos/include
)
set_target_properties(system_idl_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_dependencies(system_idl_test naos_system_idl)
add_test(NAME system_idl_test COMMAND system_idl_test)

add_test(NAME system_idl_metadata_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/system_idl_metadata_test.py
            ${CMAKE_BINARY_DIR}/naos/naos/generated/system)

add_test(NAME idl_output_location_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/idl_output_location_test.py
            ${PROJECT_SOURCE_DIR}/naos/idl/system ${CMAKE_BINARY_DIR}/naos/naos/generated/system)

add_test(NAME naoidl_template_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/naoidl_template_test.py
            ${PROJECT_SOURCE_DIR})

add_executable(invocation_deadline_test invocation_deadline_test.cc)
target_include_directories(invocation_deadline_test PRIVATE
    ${PROJECT_SOURCE_DIR}/naos/freelibcxx/include
    ${PROJECT_SOURCE_DIR}/naos/includes
    ${PROJECT_SOURCE_DIR}/naos/include
)
set_target_properties(invocation_deadline_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME invocation_deadline_test COMMAND invocation_deadline_test)

add_executable(system_binding_contract_test system_binding_contract_test.cc)
target_include_directories(system_binding_contract_test PRIVATE
    ${CMAKE_BINARY_DIR}/naos
    ${PROJECT_SOURCE_DIR}/naos/freelibcxx/include
    ${PROJECT_SOURCE_DIR}/naos/includes
    ${PROJECT_SOURCE_DIR}/naos/include
)
set_target_properties(system_binding_contract_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_dependencies(system_binding_contract_test naos_system_idl)
add_test(NAME system_binding_contract_test COMMAND system_binding_contract_test)
