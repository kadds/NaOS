
file(GLOB_RECURSE DIR_SRCS CONFIGURE_DEPENDS *.cc *.cpp *.CC *.CPP)
file(GLOB_RECURSE DIR_SRCS_S CONFIGURE_DEPENDS *.S)

if(NAOS_TEST_ACPI_FACS_RESERVATION_COLLISION)
    # Swap the production no-op provider for the boot-test fault fixture.
    list(REMOVE_ITEM DIR_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/arch/acpi/facs_reservation_boot_test_hook_noop.cc")
    list(APPEND DIR_SRCS "${PROJECT_SOURCE_DIR}/tests/acpi_facs_reservation_collision_hook.cc")
endif()

SET(DIR_ALL ${DIR_SRCS} ${DIR_SRCS_S})

set(KERNEL_CXX_FLAGS "-mno-red-zone -fvisibility-inlines-hidden \
    -Wframe-larger-than=1024 -fno-bounds-check \
    -mno-sse -mno-sse2 -mno-avx -fno-omit-frame-pointer \
    -mno-omit-leaf-frame-pointer")

set_source_files_properties(${DIR_SRCS_S} PROPERTIES COMPILE_FLAGS "${ASM_X64}")
set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS 
  "${CXX_X64} ${FREE_CXX_FLAGS} ${KERNEL_CXX_FLAGS}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system)
add_definitions(-DOS_KERNEL)
add_executable(kernel ${DIR_ALL})
target_include_directories(kernel PRIVATE
    ${PROJECT_SOURCE_DIR}/naos/includes
    ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}
    ${PROJECT_SOURCE_DIR}/naos/include
)
target_link_libraries(kernel freelibcxx)
target_link_libraries(kernel naos_ipc_core_kernel)
if(NAOS_TEST_ACPI_INIT_CAPABILITY_ROLLBACK)
    target_compile_definitions(kernel PRIVATE NAOS_TEST_ACPI_INIT_CAPABILITY_ROLLBACK=1)
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE)
    target_compile_definitions(kernel PRIVATE NAOS_TEST_ACPI_GLOBAL_LOCK_WAKE=1)
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT)
    target_compile_definitions(kernel PRIVATE NAOS_TEST_ACPI_GLOBAL_LOCK_OWNER_EXIT=1)
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE)
    target_compile_definitions(kernel PRIVATE NAOS_TEST_ACPI_GLOBAL_LOCK_RELEASE_FAILURE=1)
endif()
if(NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK)
    target_compile_definitions(kernel PRIVATE NAOS_TEST_ACPI_GLOBAL_LOCK_SOFTWARE_FALLBACK=1)
endif()

set(KERNEL_LINK_TAG "-Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/arch/kernel.ld")

set_target_properties(kernel PROPERTIES LINK_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}  ${KERNEL_CXX_FLAGS} ${LINKER_X64} \
  ${KERNEL_LINK_TAG} ${FREE_LINKER_FLAGS} -Bsymbolic")

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/kernel)

Set(To ${DEBUG_OUTPUT_DIRECTORY}/system/kernel)
add_custom_command(TARGET kernel
    POST_BUILD
    COMMAND ${UTIL_EXTRA} ${Target} ${To} i386:x86-64
)
add_custom_target(make-kernel-symbol ALL
    COMMAND ${Python3_EXECUTABLE} ${UTIL_MKSYMBOL}
            --build-dir ${CMAKE_BINARY_DIR}
            --input ${DEBUG_OUTPUT_DIRECTORY}/system/kernel.dbg
            --output ${ROOT_FS_DIR}/data/ksybs
    DEPENDS kernel)
