
set(MLIBC ${CMAKE_CURRENT_SOURCE_DIR}/mlibc)

# mlibc is bootstrapped outside this CMake project, but its archive is consumed
# by every NaOS userland target. Track its source tree and keep the external
# Ninja graph in this build graph so an incremental CMake build cannot link an
# archive compiled from stale sysdeps sources.
find_program(NAOS_NINJA_EXECUTABLE NAMES ninja REQUIRED)
file(GLOB_RECURSE NAOS_MLIBC_SOURCES CONFIGURE_DEPENDS
  ${MLIBC}/meson.build
  ${MLIBC}/meson_options.txt
  ${MLIBC}/abis/naos/*
  ${MLIBC}/include/*
  ${MLIBC}/options/*
  ${MLIBC}/sysdeps/naos/*
  ${MLIBC}/subprojects/cxxshim/*
  ${MLIBC}/subprojects/freestnd-c-hdrs/*
  ${MLIBC}/subprojects/freestnd-cxx-hdrs/*
  ${MLIBC}/subprojects/frigg/*
)
set(NAOS_MLIBC_STAMP ${CMAKE_CURRENT_BINARY_DIR}/mlibc.stamp)
add_custom_command(
  OUTPUT ${NAOS_MLIBC_STAMP}
  COMMAND ${NAOS_NINJA_EXECUTABLE} -C ${MLIBC}/build
  COMMAND ${CMAKE_COMMAND} -E touch ${NAOS_MLIBC_STAMP}
  DEPENDS ${NAOS_MLIBC_SOURCES}
  BYPRODUCTS
    ${MLIBC}/build/libc.a
    ${MLIBC}/build/libm.a
    ${MLIBC}/build/sysdeps/naos/crt1.o
  USES_TERMINAL
  VERBATIM
)
add_custom_target(naos_mlibc_build DEPENDS ${NAOS_MLIBC_STAMP})

add_library(mlibc STATIC IMPORTED GLOBAL)

set_target_properties(mlibc PROPERTIES IMPORTED_LOCATION ${MLIBC}/build/libc.a)
add_dependencies(mlibc naos_mlibc_build)

target_include_directories(mlibc SYSTEM INTERFACE 
  ${MLIBC}/build
  ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}
  ${MLIBC}/sysdeps/naos/include 
  ${MLIBC}/options/posix/include 
  ${MLIBC}/options/bsd/include 
  ${MLIBC}/options/ansi/include 
  ${MLIBC}/options/internal/include
)

target_include_directories(mlibc SYSTEM INTERFACE ${MLIBC}/subprojects/frigg/include)
# target_link_directories(mlibc PRIVATE ${CMAKE_SOURCE_DIR}/naos/libc/mlibc/build/)
target_link_directories(mlibc INTERFACE ${MLIBC}/build/sysdeps/naos)

add_library(mlibc_m STATIC IMPORTED GLOBAL)
set_target_properties(mlibc_m PROPERTIES IMPORTED_LOCATION ${MLIBC}/build/libm.a)
add_dependencies(mlibc_m naos_mlibc_build)
target_link_directories(mlibc_m INTERFACE ${MLIBC}/build)

add_library(crt1 OBJECT IMPORTED GLOBAL)
set_target_properties(crt1 PROPERTIES IMPORTED_OBJECTS ${MLIBC}/build/sysdeps/naos/crt1.o)
add_dependencies(crt1 naos_mlibc_build)
