
file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)
file(GLOB_RECURSE  DIR_SRCS_S *.S)


SET(DIR_ALL ${DIR_SRCS} ${DIR_SRCS_S})

set(KERNEL_CXX_FLAGS "-mno-red-zone -Wunreachable-code -fvisibility-inlines-hidden \
    -Wframe-larger-than=1024 -pipe -std=c++20 -Wno-pedantic \
    -fno-rtti -fno-asynchronous-unwind-tables -fno-common \
    -fno-plt -fno-exceptions -fno-pic -no-pie -Wall \
    -ffreestanding -fno-stack-protector -fno-bounds-check \
    -mno-sse -mno-sse2 -mno-avx -fno-omit-frame-pointer \
    -mno-omit-leaf-frame-pointer -fno-builtin")

set_source_files_properties(${DIR_SRCS_S} PROPERTIES COMPILE_FLAGS "${ASM_X64}")
set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64} ${KERNEL_CXX_FLAGS}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system)
add_definitions(-DOS_KERNEL)
add_executable(kernel ${DIR_ALL})

set_target_properties(kernel PROPERTIES LINK_FLAGS "${LINKER_X64} -Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/arch/kernel.ld")

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/kernel)

Set(To ${DEBUG_OUTPUT_DIRECTORY}/system/kernel)
add_custom_command(TARGET kernel
    POST_BUILD
    COMMAND ${UTIL_EXTRA} ${Target} ${To} i386:x86-64
)
add_custom_target(make-kernel-symbol ALL COMMAND python ${UTIL_MKSYMBOL} DEPENDS kernel)
