file(GLOB NAOS_IPC_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc)

set_source_files_properties(${NAOS_IPC_CORE_SOURCES}
    PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")

add_library(naos_ipc_core STATIC ${NAOS_IPC_CORE_SOURCES})
target_include_directories(naos_ipc_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/naos/include)

# The same transport-neutral sources are linked into the freestanding kernel.
# Keep that build separate from the hosted test archive so the kernel object
# files are compiled with its no-red-zone/no-SSE ABI instead of inheriting the
# hosted compiler defaults.
add_library(naos_ipc_core_kernel STATIC ${NAOS_IPC_CORE_SOURCES})
target_include_directories(naos_ipc_core_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/naos/include)
target_compile_options(naos_ipc_core_kernel PRIVATE
    -mno-red-zone
    -fvisibility-inlines-hidden
    -Wframe-larger-than=1024
    -fno-bounds-check
    -mno-sse
    -mno-sse2
    -mno-avx
    -fno-omit-frame-pointer
    -mno-omit-leaf-frame-pointer)

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
