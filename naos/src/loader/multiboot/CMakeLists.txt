
file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)
file(GLOB_RECURSE  DIR_SRCS_S *.S)

SET(DIR_ALL ${DIR_SRCS} ${DIR_SRCS_S})

set_source_files_properties(${DIR_SRCS_S} PROPERTIES COMPILE_FLAGS "${ASM_X86}")
set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X86}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/loader)


add_executable(loader-multiboot ${DIR_ALL})
set_target_properties(loader-multiboot PROPERTIES LINK_FLAGS "${LINKER_X86} -Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/loader-i386.ld")
target_link_libraries(loader-multiboot PUBLIC loader-common)

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/loader-multiboot)
Set(To ${DEBUG_OUTPUT_DIRECTORY}/loader/loader-multiboot)
add_custom_command(TARGET loader-multiboot
    POST_BUILD
    COMMAND ${UTIL_EXTRA} ${Target} ${To} i386
)