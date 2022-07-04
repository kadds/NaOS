# function(add_bin name)
#     file(GLOB_RECURSE  DIR_SRCS "${name}.cc")

#     SET(DIR_ALL ${DIR_SRCS})

#     set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64}")
#     set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

#     add_executable(${name} ${DIR_ALL})

#     target_link_libraries(${name} PUBLIC usr-common)

#     Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${name})
#     Set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/${name})

#     add_custom_command(TARGET ${name}
#         POST_BUILD
#         COMMAND ${UTIL_STRIP} ${Target} ${To} 
#     )
# endfunction(add_bin name)

add_subdirectory(common)
# add_subdirectory(init)
# add_subdirectory(syscall_test)
add_subdirectory(bin)

add_custom_target(make-rfs-image ALL COMMAND python ${UTIL_MKIMG} DEPENDS kernel nanobox make-kernel-symbol)
