cmake_minimum_required (VERSION 3.3)

project (Naos C CXX ASM)
ENABLE_LANGUAGE( ASM)

enable_testing ()


set(ASM_X64 "-mcmodel=large -Wa,--64")
if (NOT CMAKE_COMPILER_IS_GNUCXX)
    # clang
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-command-line-argument -Wno-unknown-warning-option -Wno-unused-const-variable")
    set(ASM_X64 "-mcmodel=large")
endif()

set(LINKER_X86 "-Wl,-n -Wl,-melf_i386 -Wl,--no-relax -Wl,--build-id=none")
set(ASM_X86 "-Wa,--32")
set(CXX_X86 "-m32")

set(LINKER_X64 "-Wl,-n -Wl,-melf_x86_64 -Wl,--no-relax -Wl,--build-id=none")

set(CXX_X64 "-mcmodel=large -m64")

set(CMAKE_CXX_FLAGS_DEBUG "-g -Og")
set(CMAKE_ASM_FLAGS_DEBUG "-g -Og")

set(CMAKE_CXX_FLAGS_RELEASE "-g -Os")
set(CMAKE_ASM_FLAGS_RELEASE "-g -O2")

# dirs
set(CMAKE_BINARY_DIR ${PROJECT_SOURCE_DIR}/build)
set(ROOT_FS_DIR ${CMAKE_BINARY_DIR}/bin/rfsroot)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(DEBUG_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/debug)

set(FREE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wunreachable-code -pipe -std=c++20 -Wno-pedantic \
-fno-rtti -fno-asynchronous-unwind-tables -fno-common \
-fno-plt -fno-exceptions -fno-pic -no-pie -Wall \
-ffreestanding -fno-stack-protector -fno-builtin")

set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -fno-plt")
set(FREE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib -nostartfiles -nodefaultlibs")

set(UTILDIR ${PROJECT_SOURCE_DIR}/util)
set(UTIL_EXTRA ${UTILDIR}/extra.sh)
set(UTIL_STRIP ${UTILDIR}/strip.sh)
set(UTIL_LN ${UTILDIR}/ln.sh)
set(UTIL_MKIMG ${UTILDIR}/pack.py)
set(UTIL_MKSYMBOL ${UTILDIR}/make_symbols.py)

set(UEFI_BASE_INCLUDES "/usr/include/efi/;/usr/include/efi/x86_64/")

if (CMAKE_BUILD_TYPE STREQUAL Debug)
    add_definitions(
        -D_DEBUG
    )
else()
    set (FREE_CXX_FLAGS "${FREE_CXX_FLAGS} -flto")
    # set (FREE_CXX_FLAGS "${FREE_CXX_FLAGS} -flto -fno-align-jumps -fno-align-functions \
    # -fno-align-labels  -fno-align-loops -freorder-blocks-algorithm=simple -fno-prefetch-loop-arrays")
endif ()

string(TIMESTAMP BUILD_VERSION_TS "%s")
add_definitions(-DBUILD_VERSION_TS=${BUILD_VERSION_TS})

add_subdirectory(naos)