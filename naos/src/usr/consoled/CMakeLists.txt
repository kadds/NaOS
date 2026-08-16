file(GLOB_RECURSE DIR_SRCS *.cc *.cpp *.CC *.CPP)

SET(DIR_ALL ${DIR_SRCS})

set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

add_executable(consoled ${DIR_ALL})
add_dependencies(consoled naos_system_idl libvterm_naos)
target_include_directories(consoled PRIVATE
    ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}
    ${PROJECT_SOURCE_DIR}/naos/include
)

set(EXTEND_LDS "")
if(CMAKE_COMPILER_IS_GNUCXX)
  set(EXTEND_LDS "-Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/../link.ld")
endif()

set_target_properties(consoled PROPERTIES LINK_FLAGS
    "${CXX_X64} ${FREE_CXX_FLAGS} ${LINKER_X64} ${FREE_LINKER_FLAGS} ${EXTEND_LDS}")
target_link_libraries(consoled libvterm_naos mlibc crt1)

set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/consoled)
set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/consoled)

add_custom_command(TARGET consoled POST_BUILD
    COMMAND ${UTIL_STRIP} ${Target} ${To}
)
