
file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)

SET(OBJS
  ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/sysdeps/naos/crt1.o
)

SET(DIR_ALL ${DIR_SRCS})

set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

set(MLIBC ${CMAKE_SOURCE_DIR}/naos/libc/mlibc)
add_executable(init ${DIR_ALL} ${OBJS})

target_include_directories(init SYSTEM PUBLIC ${MLIBC}/build/ ${MLIBC}/sysdeps/naos/include 
  ${MLIBC}/options/posix/include ${MLIBC}/options/ansi/include ${MLIBC}/options/internal/include)
target_include_directories(init SYSTEM PUBLIC  ${MLIBC}/subprojects/frigg/include)

target_link_directories(init PRIVATE ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/)
target_link_directories(init PRIVATE ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/sysdeps/naos)

set(EXTEND_LDS "")

if(CMAKE_COMPILER_IS_GNUCXX)
  set(EXTEND_LDS "-Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/../link.ld")
endif()

set_target_properties(init PROPERTIES LINK_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS} ${LINKER_X64} ${FREE_LINKER_FLAGS} ${EXTEND_LDS}")
target_link_libraries(init c)

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/init)
Set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/init)

add_custom_command(TARGET init
    POST_BUILD
    COMMAND ${UTIL_STRIP} ${Target} ${To} 
)