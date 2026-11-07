cmake_minimum_required (VERSION 3.9)

project (Naos C CXX ASM)
ENABLE_LANGUAGE( ASM)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Keep the standard QEMU EDU route suite available in developer builds. Release
# images stay production-like unless the smoke is explicitly requested.
set(NAOS_PCIED_MSI_ROUTE_SMOKE_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(NAOS_PCIED_MSI_ROUTE_SMOKE_DEFAULT ON)
endif()
option(NAOS_PCIED_MSI_ROUTE_SMOKE
    "Build pcied with the QEMU EDU MSI route-prepare/teardown smoke"
    ${NAOS_PCIED_MSI_ROUTE_SMOKE_DEFAULT})
unset(NAOS_PCIED_MSI_ROUTE_SMOKE_DEFAULT)
option(NAOS_PCIED_VIRTIO_BLOCK_FAULT_SMOKE
    "Inject one unmapped DMA access in virtioblkd for its opt-in terminal-cleanup smoke"
    OFF)
option(NAOS_TEST_AMDVI_MEMORY_IOPF
    "Expect an AMD-spec-conforming QEMU to deliver a memory I/O page fault in the EDU smoke"
    OFF)
option(NAOS_TEST_ACPI_FACS_RESERVATION_COLLISION
    "Build the negative QEMU boot test with an isolated FACS reservation fixture"
    OFF)
option(NAOS_TEST_ACPI_INIT_CAPABILITY_ROLLBACK
    "Fail ACPI initialization after private capability construction to verify atomic rollback"
    OFF)
option(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE
    "Exercise the QEMU PM1 GBL_STS/SCI waiter path with an external FACS Global Lock holder"
    OFF)
option(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT
    "Exit platformd while its AML worker owns the FACS Global Lock to test kernel owner teardown"
    OFF)
option(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE
    "Inject a FACS Global Lock release failure to verify sticky fail-closed behavior"
    OFF)
option(NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK
    "Force local software Global Lock arbitration for its opt-in QEMU boot test"
    OFF)

if(NAOS_PCIED_VIRTIO_BLOCK_FAULT_SMOKE AND NOT NAOS_PCIED_MSI_ROUTE_SMOKE)
    message(FATAL_ERROR "NAOS_PCIED_VIRTIO_BLOCK_FAULT_SMOKE requires NAOS_PCIED_MSI_ROUTE_SMOKE")
endif()
if(NAOS_TEST_AMDVI_MEMORY_IOPF AND NOT NAOS_PCIED_MSI_ROUTE_SMOKE)
    message(FATAL_ERROR "NAOS_TEST_AMDVI_MEMORY_IOPF requires NAOS_PCIED_MSI_ROUTE_SMOKE")
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE AND NOT NAOS_PCIED_MSI_ROUTE_SMOKE)
    message(FATAL_ERROR "NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE requires NAOS_PCIED_MSI_ROUTE_SMOKE")
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT AND NOT NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE)
    message(FATAL_ERROR "NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT requires NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE")
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE AND NOT NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE)
    message(FATAL_ERROR "NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE requires NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE")
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT AND NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
    message(FATAL_ERROR "Global Lock owner-exit and release-failure smokes require separate builds")
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK AND
   (NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE OR NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT OR NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE))
    message(FATAL_ERROR "Global Lock software-fallback smoke requires a hardware-event test-free build")
endif()

enable_testing ()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


set(ASM_X64 "-mcmodel=large -Wa,--64")
if (NOT CMAKE_COMPILER_IS_GNUCXX)
    # clang
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-command-line-argument -Wno-unknown-warning-option -Wno-unused-const-variable")
    set(ASM_X64 "-mcmodel=large -m64")
endif()

set(LINKER_X86 "-Wl,-n -Wl,-melf_i386 -Wl,--no-relax -Wl,--build-id=none")
set(ASM_X86 "-Wa,--32")
set(CXX_X86 "-m32")

set(LINKER_X64 "-Wl,-n -Wl,-melf_x86_64 -Wl,--no-relax -Wl,--build-id=none")

set(CXX_X64 "-mcmodel=large -m64")

set(CMAKE_CXX_FLAGS_DEBUG "-g ")
set(CMAKE_ASM_FLAGS_DEBUG "-g -Og")

set(CMAKE_CXX_FLAGS_RELEASE "-g -Os")
set(CMAKE_ASM_FLAGS_RELEASE "-g -O2")

# dirs
set(ROOT_FS_DIR ${CMAKE_BINARY_DIR}/bin/rfsroot)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(DEBUG_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/debug)

set(FREE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wunreachable-code -pipe -std=c++23 -Wno-pedantic \
-fno-rtti -fno-asynchronous-unwind-tables -fno-common \
-fno-plt -fno-exceptions -fno-pic -no-pie -Wall \
-ffreestanding -fno-stack-protector -fno-builtin -U__linux__ -U_LINUX")

set(FREE_C_FLAGS "${CMAKE_CXX_FLAGS} -Wunreachable-code -pipe -std=c11 -Wno-pedantic \
-fno-asynchronous-unwind-tables -fno-common \
-fno-plt -fno-exceptions -fno-pic -no-pie -Wall \
-ffreestanding -fno-stack-protector -fno-builtin -U__linux__ -U_LINUX")

set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -fno-plt")
set(FREE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib -nostartfiles -nodefaultlibs")
add_definitions(-DNAOS)

set(UTILDIR ${PROJECT_SOURCE_DIR}/util)
set(UTIL_EXTRA ${UTILDIR}/extra.sh)
set(UTIL_STRIP ${UTILDIR}/strip.sh)
set(UTIL_LN ${UTILDIR}/ln.sh)
set(UTIL_MKSYMBOL ${UTILDIR}/make_symbols.py)
set(UTIL_BUSYBOX_CONFIG ${UTILDIR}/config_busybox.sh)

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
add_subdirectory(tests)
