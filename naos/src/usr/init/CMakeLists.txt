
file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)
file(GLOB_RECURSE  DIR_SRCS_S *.S)

SET(OBJS
  ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/sysdeps/naos/crt1.o
)

SET(DIR_ALL ${DIR_SRCS} ${DIR_SRCS_S})

set_source_files_properties(${DIR_SRCS_S} PROPERTIES COMPILE_FLAGS "${ASM_X64}")
set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

set(MLIBC ${CMAKE_SOURCE_DIR}/naos/libc/mlibc)
add_executable(init ${DIR_ALL} ${OBJS})

target_include_directories(init SYSTEM PUBLIC ${MLIBC}/build/ ${MLIBC}/sysdeps/naos/include 
  ${MLIBC}/options/posix/include ${MLIBC}/options/ansi/include ${MLIBC}/options/internal/include)

target_link_directories(init PRIVATE ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/)
target_link_directories(init PRIVATE ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/sysdeps/naos)

target_link_libraries(init c)

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/init)
Set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/init)

add_custom_command(TARGET init
    POST_BUILD
    COMMAND ${UTIL_STRIP} ${Target} ${To} 
)