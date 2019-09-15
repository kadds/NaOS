file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)
file(GLOB_RECURSE  DIR_SRCS_S *.S)

SET(DIR_ALL ${DIR_SRCS} ${DIR_SRCS_S})

set_source_files_properties(${DIR_SRCS_S} PROPERTIES COMPILE_FLAGS "${ASM_X64}")
set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64}")

add_library(usr-common STATIC ${DIR_ALL})
set_target_properties(usr-common PROPERTIES LINK_FLAGS "${LINKER_X64}")

