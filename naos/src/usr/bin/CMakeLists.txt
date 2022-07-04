
file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)
SET(OBJS
  ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/sysdeps/naos/crt0.o
)

add_executable(nanobox ${OBJS} ${DIR_SRCS} )
target_include_directories(nanobox SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/naos/libc/mlib/options/posix/include)
target_link_directories(nanobox PRIVATE ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/)
target_link_directories(nanobox PRIVATE ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/sysdeps/naos)

# target_link_libraries(nanobox crt0)
target_link_libraries(nanobox c)
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