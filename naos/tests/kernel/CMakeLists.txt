file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)
file(GLOB_RECURSE  DIR_SRCS_S *.S)
set(BEG "${PROJECT_SOURCE_DIR}/naos/src/kernel")
SET(DIR_SRCS ${DIR_SRCS} "${BEG}/util/str.cc")
SET(DIR_SRCS ${DIR_SRCS} "${BEG}/util/memory.cc")
SET(DIR_SRCS ${DIR_SRCS} "${BEG}/util/hash.cc")

SET(DIR_ALL ${DIR_SRCS} ${DIR_SRCS_S})

set(CMAKE_CXX_FLAGS "-fsanitize=address -std=c++17 -g -fno-inline-small-functions")
set(LINKER_X64 "-Wl,-n -Wl,-melf_x86_64")
set(CMAKE_EXE_LINKER_FLAGS "-fsanitize=address")

set_source_files_properties(${DIR_SRCS_S} PROPERTIES COMPILE_FLAGS "${ASM_X64}")
set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/ktest)

add_executable(ktest ${DIR_ALL})
add_test(ktest ${DIR_ALL})
