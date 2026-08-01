
set(MLIBC ${CMAKE_CURRENT_SOURCE_DIR}/mlibc)
add_library(mlibc STATIC IMPORTED GLOBAL)

set_target_properties(mlibc PROPERTIES IMPORTED_LOCATION ${MLIBC}/build/libc.a)

target_include_directories(mlibc SYSTEM INTERFACE 
  ${MLIBC}/build
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
target_link_directories(mlibc_m INTERFACE ${MLIBC}/build)

add_library(crt1 OBJECT IMPORTED GLOBAL)
set_target_properties(crt1 PROPERTIES IMPORTED_OBJECTS ${MLIBC}/build/sysdeps/naos/crt1.o)
