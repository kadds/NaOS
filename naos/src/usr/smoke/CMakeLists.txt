set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

add_executable(naos-smoke-suite main.cc mlibc_tls.cc)
set_source_files_properties(main.cc mlibc_tls.cc PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")
target_include_directories(naos-smoke-suite PRIVATE ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR} ${PROJECT_SOURCE_DIR}/naos/include)
set_target_properties(naos-smoke-suite PROPERTIES
    LINK_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS} ${LINKER_X64} ${FREE_LINKER_FLAGS} -Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/../link.ld")
target_link_libraries(naos-smoke-suite naos_rust_tls_probe mlibc crt1)

add_custom_target(naos_smoke_suite ALL DEPENDS
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/naos-smoke-suite)

add_custom_command(TARGET naos-smoke-suite POST_BUILD
    COMMAND ${UTIL_STRIP} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/naos-smoke-suite
            ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/naos-smoke-suite)
