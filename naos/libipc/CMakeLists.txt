file(GLOB NAOS_IPC_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc)

set_source_files_properties(${NAOS_IPC_CORE_SOURCES}
    PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")

add_library(naos_ipc_core STATIC ${NAOS_IPC_CORE_SOURCES})
target_include_directories(naos_ipc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/naos/include)

find_package(Threads REQUIRED)
add_executable(naos_ipc_core_test tests/channel_test.cc)
target_include_directories(naos_ipc_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(naos_ipc_core_test PRIVATE naos_ipc_core Threads::Threads)
add_test(NAME naos_ipc_core_test COMMAND naos_ipc_core_test)

add_executable(naos_ipc_core_c_api_test tests/c_api_test.cc)
target_include_directories(naos_ipc_core_c_api_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(naos_ipc_core_c_api_test PRIVATE naos_ipc_core Threads::Threads)
add_test(NAME naos_ipc_core_c_api_test COMMAND naos_ipc_core_c_api_test)

add_executable(naos_ipc_core_invocation_test tests/invocation_test.cc)
target_include_directories(naos_ipc_core_invocation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(naos_ipc_core_invocation_test PRIVATE naos_ipc_core Threads::Threads)
add_test(NAME naos_ipc_core_invocation_test COMMAND naos_ipc_core_invocation_test)

add_executable(naos_ipc_core_c_header_test tests/c_api_header_test.c)
target_include_directories(naos_ipc_core_c_header_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(naos_ipc_core_c_header_test PRIVATE naos_ipc_core)
add_test(NAME naos_ipc_core_c_header_test COMMAND naos_ipc_core_c_header_test)

add_executable(naos_ipc_core_ring_test tests/ring_test.cc)
target_include_directories(naos_ipc_core_ring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(naos_ipc_core_ring_test PRIVATE naos_ipc_core Threads::Threads)
add_test(NAME naos_ipc_core_ring_test COMMAND naos_ipc_core_ring_test)

add_executable(naos_ipc_core_ring_c_api_test tests/ring_c_api_test.cc)
target_include_directories(naos_ipc_core_ring_c_api_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(naos_ipc_core_ring_c_api_test PRIVATE naos_ipc_core Threads::Threads)
add_test(NAME naos_ipc_core_ring_c_api_test COMMAND naos_ipc_core_ring_c_api_test)

add_executable(naos_ipc_core_transport_contract_test tests/transport_contract_test.cc)
target_include_directories(naos_ipc_core_transport_contract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(naos_ipc_core_transport_contract_test PRIVATE naos_ipc_core Threads::Threads)
add_test(NAME naos_ipc_core_transport_contract_test COMMAND naos_ipc_core_transport_contract_test)
