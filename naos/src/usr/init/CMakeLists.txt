file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)

SET(DIR_ALL ${DIR_SRCS})

set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

add_executable(init ${DIR_ALL})
add_dependencies(init naos_system_idl)
target_include_directories(init PRIVATE ${NAOS_SYSTEM_GENERATED_INCLUDE_DIR} ${PROJECT_SOURCE_DIR}/naos/include)

set(EXTEND_LDS "")

if(CMAKE_COMPILER_IS_GNUCXX)
  set(EXTEND_LDS "-Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/../link.ld")
endif()

set_target_properties(init PROPERTIES LINK_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS} ${LINKER_X64} ${FREE_LINKER_FLAGS} ${EXTEND_LDS}")
target_link_libraries(init mlibc crt1)

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/init)
Set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/init)

add_custom_command(TARGET init
    POST_BUILD
    COMMAND ${UTIL_STRIP} ${Target} ${To} 
)

# The final boot contract launches init as a fixed Multiboot module.  Keep the
# ordinary rfsroot copy for the archive transition until the mounted root
# backend is available, but always produce the module artifact independently.
set(NAOS_INIT_INSTALL ${CMAKE_BINARY_DIR}/bin/system/init)
add_custom_command(
    OUTPUT ${NAOS_INIT_INSTALL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin/system
    COMMAND ${CMAKE_COMMAND} -E copy ${Target} ${NAOS_INIT_INSTALL}
    DEPENDS init
    VERBATIM)
add_custom_target(init_boot_module ALL DEPENDS ${NAOS_INIT_INSTALL})
