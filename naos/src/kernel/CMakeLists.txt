
file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)
file(GLOB_RECURSE  DIR_SRCS_S *.S)


SET(DIR_ALL ${DIR_SRCS} ${DIR_SRCS_S})

set_source_files_properties(${DIR_SRCS_S} PROPERTIES COMPILE_FLAGS "${ASM_X64}")
set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system)

add_executable(kernel ${DIR_ALL})

set_target_properties(kernel PROPERTIES LINK_FLAGS "${LINKER_X64} -Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/arch/kernel.ld")

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/kernel)

Set(To ${DEBUG_OUTPUT_DIRECTORY}/system/kernel)
add_custom_command(TARGET kernel
    POST_BUILD
    COMMAND ${UTIL_EXTRA} ${Target} ${To} i386:x86-64
)
add_custom_target(make-kernel-symbol ALL COMMAND python ${UTIL_MKSYMBOL} DEPENDS kernel)
