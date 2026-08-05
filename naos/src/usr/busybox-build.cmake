set(BUSYBOX_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/busybox)
set(BUSYBOX_BUILD_DIR ${CMAKE_BINARY_DIR}/busybox)
set(BUSYBOX_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/busybox.config)
set(BUSYBOX_COMPAT_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/busybox-compat.h)
set(BUSYBOX_CRT_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/busybox-crt.c)

set(NAOS_MLIBC_DIR ${PROJECT_SOURCE_DIR}/naos/libc/mlibc)
set(NAOS_MLIBC_ARCHIVE ${NAOS_MLIBC_DIR}/build/libc.a)
set(NAOS_MLIBM_ARCHIVE ${NAOS_MLIBC_DIR}/build/libm.a)
set(NAOS_CRT1_OBJECT ${NAOS_MLIBC_DIR}/build/sysdeps/naos/crt1.o)

if(NOT EXISTS "${BUSYBOX_SOURCE_DIR}/Makefile")
    message(FATAL_ERROR "BusyBox source is missing; initialize the 1.37.0 submodule")
endif()

find_program(BUSYBOX_MAKE_PROGRAM make)
find_program(BUSYBOX_HOSTCC NAMES cc gcc)
if(NOT BUSYBOX_MAKE_PROGRAM OR NOT BUSYBOX_HOSTCC)
    message(FATAL_ERROR "BusyBox requires a host make and C compiler")
endif()

set(BUSYBOX_CFLAGS "-mcmodel=large -m64 -std=gnu99 -ffreestanding -fno-stack-protector -fno-pic -fno-plt -fno-builtin -fno-asynchronous-unwind-tables -fno-common -U__linux__ -U_LINUX -I${NAOS_SYSTEM_GENERATED_INCLUDE_DIR} -I${PROJECT_SOURCE_DIR}/naos/include -I${NAOS_MLIBC_DIR}/build -I${NAOS_MLIBC_DIR}/sysdeps/naos/include -I${NAOS_MLIBC_DIR}/options/posix/include -I${NAOS_MLIBC_DIR}/options/bsd/include -I${NAOS_MLIBC_DIR}/options/ansi/include -I${NAOS_MLIBC_DIR}/options/internal/include -I${NAOS_MLIBC_DIR}/options/linux/include -I${NAOS_MLIBC_DIR}/options/glibc/include -I${NAOS_MLIBC_DIR}/subprojects/frigg/include -idirafter ${NAOS_MLIBC_DIR}/subprojects/freestnd-c-hdrs/x86_64/include -include ${BUSYBOX_COMPAT_HEADER}")

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -print-libgcc-file-name
        OUTPUT_VARIABLE BUSYBOX_LIBGCC_ARCHIVE
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(BUSYBOX_LIBGCC_DIR "${BUSYBOX_LIBGCC_ARCHIVE}" DIRECTORY)
    set(BUSYBOX_RUNTIME_LDFLAGS "-L${BUSYBOX_LIBGCC_DIR}")
    set(BUSYBOX_RUNTIME_LIBS "gcc gcc_eh")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" --print-runtime-dir
        OUTPUT_VARIABLE BUSYBOX_COMPILER_RT_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT EXISTS "${BUSYBOX_COMPILER_RT_DIR}/libclang_rt.builtins-x86_64.a")
        message(FATAL_ERROR "Clang compiler-rt builtins for x86-64 were not found")
    endif()
    set(BUSYBOX_RUNTIME_LDFLAGS "-rtlib=compiler-rt -L${BUSYBOX_COMPILER_RT_DIR}")
    set(BUSYBOX_RUNTIME_LIBS "clang_rt.builtins-x86_64")
else()
    message(FATAL_ERROR "BusyBox requires GCC or Clang for explicit compiler runtime handling")
endif()

set(BUSYBOX_CRT_CFLAGS "-mcmodel=large -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables -fno-common -fno-builtin -U__linux__ -U_LINUX")
separate_arguments(BUSYBOX_CRT_CFLAGS_ARGS UNIX_COMMAND "${BUSYBOX_CRT_CFLAGS}")

if(CMAKE_RANLIB)
    set(BUSYBOX_RANLIB ${CMAKE_RANLIB})
else()
    find_program(BUSYBOX_RANLIB ranlib)
    if(NOT BUSYBOX_RANLIB)
        message(FATAL_ERROR "BusyBox requires ranlib to create its CRT archive")
    endif()
endif()

set(BUSYBOX_CRT_OBJECT ${BUSYBOX_BUILD_DIR}/busybox-crt.o)
set(BUSYBOX_CRT_ARCHIVE ${BUSYBOX_BUILD_DIR}/libbusyboxcrt.a)
add_custom_command(
    OUTPUT ${BUSYBOX_CRT_ARCHIVE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BUSYBOX_BUILD_DIR}
    COMMAND ${CMAKE_C_COMPILER} ${BUSYBOX_CRT_CFLAGS_ARGS} -c ${BUSYBOX_CRT_SOURCE} -o ${BUSYBOX_CRT_OBJECT}
    COMMAND ${CMAKE_AR} rcs ${BUSYBOX_CRT_ARCHIVE} ${BUSYBOX_CRT_OBJECT} ${NAOS_CRT1_OBJECT}
    COMMAND ${BUSYBOX_RANLIB} ${BUSYBOX_CRT_ARCHIVE}
    DEPENDS ${BUSYBOX_CRT_SOURCE} ${NAOS_CRT1_OBJECT}
    VERBATIM)

set(BUSYBOX_ROOTFS_BINARY ${ROOT_FS_DIR}/bin/busybox)
set(BUSYBOX_UNSTRIPPED_BINARY ${BUSYBOX_BUILD_DIR}/busybox_unstripped)
set(BUSYBOX_DEBUG_BINARY ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/busybox)
set(BUSYBOX_EXTRA_LDFLAGS "-nostdlib -nostartfiles -nodefaultlibs -static ${LINKER_X64} -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/link.ld -Wl,-u,main -L${BUSYBOX_BUILD_DIR} -L${NAOS_MLIBC_DIR}/build ${BUSYBOX_RUNTIME_LDFLAGS}")
set(BUSYBOX_EXTRA_LDLIBS "busyboxcrt c m ${BUSYBOX_RUNTIME_LIBS}")

add_custom_command(
    OUTPUT ${BUSYBOX_ROOTFS_BINARY}
    COMMAND ${UTIL_BUSYBOX_CONFIG} ${BUSYBOX_SOURCE_DIR} ${BUSYBOX_BUILD_DIR} ${BUSYBOX_CONFIG} ${BUSYBOX_MAKE_PROGRAM} ${BUSYBOX_HOSTCC}
    COMMAND ${BUSYBOX_MAKE_PROGRAM} -C ${BUSYBOX_SOURCE_DIR}
        O=${BUSYBOX_BUILD_DIR}
        ARCH=x86_64
        HOSTCC=${BUSYBOX_HOSTCC}
        CC=${CMAKE_C_COMPILER}
        AR=${CMAKE_AR}
        CFLAGS=${BUSYBOX_CFLAGS}
        CONFIG_EXTRA_LDFLAGS=${BUSYBOX_EXTRA_LDFLAGS}
        CONFIG_EXTRA_LDLIBS=${BUSYBOX_EXTRA_LDLIBS}
        busybox_unstripped
    COMMAND ${UTIL_STRIP} ${BUSYBOX_UNSTRIPPED_BINARY} ${BUSYBOX_DEBUG_BINARY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ROOT_FS_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E copy ${BUSYBOX_UNSTRIPPED_BINARY} ${BUSYBOX_ROOTFS_BINARY}
    DEPENDS ${BUSYBOX_CONFIG} ${BUSYBOX_COMPAT_HEADER} ${BUSYBOX_CRT_ARCHIVE}
        ${UTIL_BUSYBOX_CONFIG} ${UTIL_STRIP} ${CMAKE_CURRENT_SOURCE_DIR}/link.ld
        ${NAOS_MLIBC_ARCHIVE} ${NAOS_MLIBM_ARCHIVE}
        ${BUSYBOX_SOURCE_DIR}/Makefile
    VERBATIM)

add_custom_target(busybox ALL DEPENDS ${BUSYBOX_ROOTFS_BINARY})
