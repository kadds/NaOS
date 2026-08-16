file(GLOB_RECURSE DIR_SRCS *.cc *.cpp *.CC *.CPP)

SET(DIR_ALL ${DIR_SRCS})

set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

add_executable(ttyd ${DIR_ALL})
add_dependencies(ttyd naos_system_idl)
target_compile_options(ttyd PRIVATE -U_GLIBCXX_ASSERTIONS)
target_include_directories(ttyd PRIVATE
    ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}
    ${PROJECT_SOURCE_DIR}/naos/include
)

set(EXTEND_LDS "")
if(CMAKE_COMPILER_IS_GNUCXX)
  set(EXTEND_LDS "-Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/../link.ld")
endif()

set_target_properties(ttyd PROPERTIES LINK_FLAGS
    "${CXX_X64} ${FREE_CXX_FLAGS} ${LINKER_X64} ${FREE_LINKER_FLAGS} ${EXTEND_LDS}")
target_link_libraries(ttyd mlibc crt1)

set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ttyd)
set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/ttyd)

add_custom_command(TARGET ttyd POST_BUILD
    COMMAND ${UTIL_STRIP} ${Target} ${To}
)
