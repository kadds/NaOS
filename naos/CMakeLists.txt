find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(NAOS_SYSTEM_IDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../idl/system)
set(NAOS_SYSTEM_GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(NAOS_SYSTEM_GENERATED_DIR ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}/naos/generated/system)
set(NAOS_SYSTEM_GENERATED_INDEX ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR}/naos/generated/system_uapi.h)
file(MAKE_DIRECTORY ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR})
set(NAOS_SYSTEM_IDL_STAMP ${CMAKE_CURRENT_BINARY_DIR}/system-idl.stamp)
file(GLOB NAOS_NAOIDL_TEMPLATES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../util/templates/naoidl/*.htt)
set(NAOS_SYSTEM_IDL_SOURCES
    ${NAOS_SYSTEM_IDL_DIR}/directory.naidl
    ${NAOS_SYSTEM_IDL_DIR}/file.naidl
    ${NAOS_SYSTEM_IDL_DIR}/memory_object.naidl
    ${NAOS_SYSTEM_IDL_DIR}/process.naidl
    ${NAOS_SYSTEM_IDL_DIR}/shared_ring.naidl
    ${NAOS_SYSTEM_IDL_DIR}/service_directory.naidl
    ${NAOS_SYSTEM_IDL_DIR}/stream.naidl
    ${NAOS_SYSTEM_IDL_DIR}/tty_control.naidl
)
add_custom_command(
    OUTPUT ${NAOS_SYSTEM_IDL_STAMP}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../util/naoidl.py check-system
            ${NAOS_SYSTEM_IDL_DIR} ${NAOS_SYSTEM_GENERATED_DIR} --allow-missing
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../util/naoidl.py generate-system
            ${NAOS_SYSTEM_IDL_DIR} ${NAOS_SYSTEM_GENERATED_DIR} ${NAOS_SYSTEM_GENERATED_INDEX}
            ${NAOS_SYSTEM_IDL_STAMP}
    DEPENDS ${NAOS_SYSTEM_IDL_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../util/naoidl.py ${NAOS_NAOIDL_TEMPLATES}
    VERBATIM
)
add_custom_target(naos_system_idl DEPENDS ${NAOS_SYSTEM_IDL_STAMP})

add_subdirectory(freelibcxx)
add_subdirectory(libc)
add_subdirectory(src/acpica)

add_subdirectory(src/kernel)
add_dependencies(kernel naos_system_idl)
add_subdirectory(src/module)
add_subdirectory(src/usr)
