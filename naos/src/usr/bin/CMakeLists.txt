
file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)

SET(OBJS
  ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/sysdeps/naos/crt1.o
)

SET(DIR_ALL ${DIR_SRCS})

set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

set(MLIBC ${CMAKE_SOURCE_DIR}/naos/libc/mlibc)

add_executable(nanobox ${OBJS} ${DIR_ALL})
target_include_directories(nanobox SYSTEM PUBLIC ${MLIBC}/build/ ${MLIBC}/sysdeps/naos/include 
  ${MLIBC}/options/posix/include ${MLIBC}/options/ansi/include ${MLIBC}/options/internal/include)

target_link_directories(nanobox PUBLIC ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/sysdeps/naos)
target_link_directories(nanobox PUBLIC ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/)
set_target_properties(nanobox PROPERTIES LINK_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS} ${LINKER_X64} ${FREE_LINKER_FLAGS}")

target_link_libraries(nanobox c freelibcxx)

# target_link_libraries(nanobox ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/ld.a)
# target_link_libraries(nanobox ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/libcrypt.a)
# target_link_libraries(nanobox ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/libm.a)
# target_link_libraries(nanobox ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/libpthread.a)
# target_link_libraries(nanobox ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/libresolv.a)
# target_link_libraries(nanobox ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/librt.a)
# target_link_libraries(nanobox ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/libutil.a)

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/nanobox)
Set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/nanobox)

add_custom_command(TARGET nanobox
    POST_BUILD
    COMMAND ${UTIL_STRIP} ${Target} ${To} 
)