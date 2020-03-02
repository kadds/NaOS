
file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)
file(GLOB_RECURSE  DIR_SRCS_S *.S)


SET(DIR_ALL ${DIR_SRCS} ${DIR_SRCS_S})

set_source_files_properties(${DIR_SRCS_S} PROPERTIES COMPILE_FLAGS "${ASM_X64}")
set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

add_executable(cat ${DIR_ALL})

target_link_libraries(cat PUBLIC usr-common)

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/cat)
Set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/cat)

add_custom_command(TARGET cat
    POST_BUILD
    COMMAND ${UTIL_STRIP} ${Target} ${To} 
)