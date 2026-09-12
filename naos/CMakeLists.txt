find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(NAOS_PERL_EXECUTABLE perl REQUIRED)

set(NAOS_LIBVTERM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm)
set(NAOS_LIBVTERM_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/libvterm_naos_generated)
if(NOT EXISTS ${NAOS_LIBVTERM_SOURCE_DIR}/src/encoding.c)
    message(FATAL_ERROR
        "libvterm submodule is missing at ${NAOS_LIBVTERM_SOURCE_DIR}; "
        "run `git submodule update --init --recursive` before configuring NaOS")
endif()
file(MAKE_DIRECTORY ${NAOS_LIBVTERM_GENERATED_DIR}/encoding)
set(NAOS_LIBVTERM_ENCODING_SOURCES
    ${NAOS_LIBVTERM_GENERATED_DIR}/encoding/DECdrawing.inc
    ${NAOS_LIBVTERM_GENERATED_DIR}/encoding/uk.inc
)
add_custom_command(
    OUTPUT ${NAOS_LIBVTERM_GENERATED_DIR}/encoding/DECdrawing.inc
    COMMAND ${NAOS_PERL_EXECUTABLE} -CSD
            ${NAOS_LIBVTERM_SOURCE_DIR}/tbl2inc_c.pl
            ${NAOS_LIBVTERM_SOURCE_DIR}/src/encoding/DECdrawing.tbl
            > ${NAOS_LIBVTERM_GENERATED_DIR}/encoding/DECdrawing.inc
    DEPENDS ${NAOS_LIBVTERM_SOURCE_DIR}/tbl2inc_c.pl
            ${NAOS_LIBVTERM_SOURCE_DIR}/src/encoding/DECdrawing.tbl
    VERBATIM
)
add_custom_command(
    OUTPUT ${NAOS_LIBVTERM_GENERATED_DIR}/encoding/uk.inc
    COMMAND ${NAOS_PERL_EXECUTABLE} -CSD
            ${NAOS_LIBVTERM_SOURCE_DIR}/tbl2inc_c.pl
            ${NAOS_LIBVTERM_SOURCE_DIR}/src/encoding/uk.tbl
            > ${NAOS_LIBVTERM_GENERATED_DIR}/encoding/uk.inc
    DEPENDS ${NAOS_LIBVTERM_SOURCE_DIR}/tbl2inc_c.pl
            ${NAOS_LIBVTERM_SOURCE_DIR}/src/encoding/uk.tbl
    VERBATIM
)
file(GLOB NAOS_LIBVTERM_SOURCES CONFIGURE_DEPENDS
    ${NAOS_LIBVTERM_SOURCE_DIR}/src/*.c)
set_source_files_properties(${NAOS_LIBVTERM_SOURCES}
    PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_C_FLAGS}")
add_library(libvterm_naos STATIC ${NAOS_LIBVTERM_SOURCES} ${NAOS_LIBVTERM_ENCODING_SOURCES})
target_include_directories(libvterm_naos PUBLIC ${NAOS_LIBVTERM_SOURCE_DIR}/include)
target_include_directories(libvterm_naos PRIVATE
    ${NAOS_LIBVTERM_SOURCE_DIR}/src
    ${NAOS_LIBVTERM_GENERATED_DIR}
)

set(NAOS_IDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../idl)
set(NAOS_IDL_COMPILER ${NAOS_IDL_DIR}/naoidl.py)
set(NAOS_SYSTEM_IDL_DIR ${NAOS_IDL_DIR}/system)
set(NAOS_SYSTEM_GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(NAOS_SYSTEM_GENERATED_DIR ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}/naos/generated/system)
set(NAOS_SYSTEM_GENERATED_INDEX ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}/naos/generated/system_uapi.h)
file(MAKE_DIRECTORY ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR})
set(NAOS_SYSTEM_IDL_STAMP ${CMAKE_CURRENT_BINARY_DIR}/system-idl.stamp)
file(GLOB NAOS_NAOIDL_TEMPLATES CONFIGURE_DEPENDS
    ${NAOS_IDL_DIR}/templates/cpp/naoidl/*.htt)
file(GLOB NAOS_SYSTEM_IDL_SOURCES CONFIGURE_DEPENDS
    ${NAOS_SYSTEM_IDL_DIR}/*.naidl)
file(GLOB NAOS_SYSTEM_IDL_FRAGMENT_SOURCES CONFIGURE_DEPENDS
    ${NAOS_SYSTEM_IDL_DIR}/fragments/*.naidl)

# Internal VFS protocols (NamespaceBinding / MountControl / MutationTicket):
# generated like the system schemas but into a separate output directory.
# These are NOT application SDK headers; only vfsd/workers/kernel consume
# them, and the Rust bindings deliberately never discover this directory.
set(NAOS_INTERNAL_IDL_DIR ${NAOS_IDL_DIR}/internal)
set(NAOS_INTERNAL_GENERATED_DIR ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}/naos/generated/internal)
set(NAOS_INTERNAL_GENERATED_INDEX ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}/naos/generated/internal_uapi.h)
set(NAOS_INTERNAL_IDL_STAMP ${CMAKE_CURRENT_BINARY_DIR}/internal-idl.stamp)
file(GLOB NAOS_INTERNAL_IDL_SOURCES CONFIGURE_DEPENDS
    ${NAOS_INTERNAL_IDL_DIR}/*.naidl)
file(GLOB NAOS_INTERNAL_IDL_FRAGMENT_SOURCES CONFIGURE_DEPENDS
    ${NAOS_INTERNAL_IDL_DIR}/fragments/*.naidl)
set(NAOS_INTERNAL_IDL_GENERATE_COMMANDS)
foreach(NAOS_INTERNAL_IDL_SOURCE IN LISTS NAOS_INTERNAL_IDL_SOURCES)
    list(APPEND NAOS_INTERNAL_IDL_GENERATE_COMMANDS
        COMMAND ${Python3_EXECUTABLE} ${NAOS_IDL_COMPILER} generate
                ${NAOS_INTERNAL_IDL_SOURCE} ${NAOS_INTERNAL_GENERATED_DIR})
endforeach()
add_custom_command(
    OUTPUT ${NAOS_INTERNAL_IDL_STAMP}
    COMMAND ${Python3_EXECUTABLE} ${NAOS_IDL_COMPILER} check-collisions
            ${NAOS_INTERNAL_IDL_SOURCES}
    ${NAOS_INTERNAL_IDL_GENERATE_COMMANDS}
    COMMAND ${Python3_EXECUTABLE} ${NAOS_IDL_COMPILER} generate-index
            ${NAOS_INTERNAL_IDL_DIR} ${NAOS_INTERNAL_GENERATED_INDEX} --include-subdir internal
    COMMAND ${CMAKE_COMMAND} -E touch ${NAOS_INTERNAL_IDL_STAMP}
    DEPENDS ${NAOS_INTERNAL_IDL_SOURCES} ${NAOS_INTERNAL_IDL_FRAGMENT_SOURCES}
            ${NAOS_IDL_COMPILER} ${NAOS_NAOIDL_TEMPLATES}
    VERBATIM
)
add_custom_target(naos_internal_idl DEPENDS ${NAOS_INTERNAL_IDL_STAMP})
# System IDL revisions are generated directly from the current schemas. Do not
# compare them against stale build-local manifests or retain removed wire
# fields solely for an obsolete baseline.
add_custom_command(
    OUTPUT ${NAOS_SYSTEM_IDL_STAMP}
    COMMAND ${Python3_EXECUTABLE} ${NAOS_IDL_COMPILER} generate-system
            ${NAOS_SYSTEM_IDL_DIR} ${NAOS_SYSTEM_GENERATED_DIR} ${NAOS_SYSTEM_GENERATED_INDEX}
            ${NAOS_SYSTEM_IDL_STAMP}
    DEPENDS ${NAOS_SYSTEM_IDL_SOURCES} ${NAOS_SYSTEM_IDL_FRAGMENT_SOURCES}
            ${NAOS_IDL_COMPILER} ${NAOS_NAOIDL_TEMPLATES}
    VERBATIM
)
add_custom_target(naos_system_idl DEPENDS ${NAOS_SYSTEM_IDL_STAMP})

add_subdirectory(freelibcxx)
add_subdirectory(libipc)
add_subdirectory(libc)
add_subdirectory(src/acpica)

add_subdirectory(src/kernel)
add_dependencies(kernel naos_system_idl naos_internal_idl)
add_subdirectory(src/usr)
