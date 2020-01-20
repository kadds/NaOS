cmake_minimum_required (VERSION 3.3)

project (Naos C CXX ASM)
ENABLE_LANGUAGE( ASM)

option(USE_CLANG "build kernel with clang" OFF)

# dirs
set(CMAKE_BINARY_DIR ${PROJECT_SOURCE_DIR}/build)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(DEBUG_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/debug)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mno-red-zone -Wunreachable-code -fvisibility-inlines-hidden -Wframe-larger-than=512 -pipe -std=c++17 -pedantic -fno-rtti -fno-asynchronous-unwind-tables -fno-common -fno-plt -fno-exceptions -fno-pic -no-pie -Wall -ffreestanding -fno-stack-protector -fno-bounds-check -mno-sse -mno-sse2 -Wno-pedantic -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -fno-plt")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib -nostartfiles -nodefaultlibs")

set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_ASM_FLAGS_DEBUG "-g -Og")

set(CMAKE_CXX_FLAGS_RELEASE "-g -O2 -fvisibility-inlines-hidden")
set(CMAKE_ASM_FLAGS_RELEASE "-g -O2")

set(LINKER_X86 "-Wl,-n -Wl,-melf_i386 -Wl,--no-relax -Wl,--build-id=none")
set(ASM_X86 "-Wa,--32")
set(CXX_X86 "-m32")

set(LINKER_X64 "-Wl,-n -Wl,-melf_x86_64 -Wl,--no-relax -Wl,--build-id=none")
set(ASM_X64 "-mcmodel=large -Wa,--64")
set(CXX_X64 "-mcmodel=large -m64")
set(UTILDIR ${PROJECT_SOURCE_DIR}/util)
set(UTIL_EXTRA ${UTILDIR}/extra.sh)
set(UTIL_STRIP ${UTILDIR}/strip.sh)
set(UTIL_MKIMG ${UTILDIR}/pack.py)
set(UTIL_MKSYMBOL ${UTILDIR}/make_symbols.py)

set(UEFI_BASE_INCLUDES "/usr/include/efi/;/usr/include/efi/x86_64/")

if (CMAKE_BUILD_TYPE STREQUAL Debug)

    add_definitions(
        -D_DEBUG
    )

endif ()



if (USE_CLANG)
    SET (CMAKE_C_COMPILER        "/usr/bin/clang")
    SET (CMAKE_CXX_COMPILER      "/usr/bin/clang++")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-command-line-argument -Wno-unknown-warning-option -Wno-unused-const-variable")
    SET (CMAKE_LINKER  "/usr/bin/llvm-link")
else()
    SET (CMAKE_C_COMPILER        "/usr/bin/gcc")
    SET (CMAKE_CXX_COMPILER      "/usr/bin/g++")
    SET (CMAKE_LINKER  "/usr/bin/ld")
endif(USE_CLANG)

string(TIMESTAMP BUILD_VERSION_TS "%s")
add_definitions(-DBUILD_VERSION_TS=${BUILD_VERSION_TS})

add_subdirectory(naos)