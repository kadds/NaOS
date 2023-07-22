file(GLOB_RECURSE  DIR_SRCS *.cc *.cpp *.CC *.CPP)

SET(DIR_ALL ${DIR_SRCS})

set_source_files_properties(${DIR_SRCS} PROPERTIES COMPILE_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rfsroot/bin)

add_executable(nanobox ${OBJS} ${DIR_ALL})

set(EXTEND_LDS "")

if(CMAKE_COMPILER_IS_GNUCXX)
  set(EXTEND_LDS "-Wl,-T ${CMAKE_CURRENT_SOURCE_DIR}/../link.ld")
endif()

set_target_properties(nanobox PROPERTIES LINK_FLAGS "${CXX_X64} ${FREE_CXX_FLAGS} ${LINKER_X64} ${FREE_LINKER_FLAGS} ${EXTEND_LDS}")

target_link_libraries(nanobox freelibcxx mlibc crt1)

Set(Target ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/nanobox)
Set(To ${DEBUG_OUTPUT_DIRECTORY}/rfsroot/bin/nanobox)

add_custom_command(TARGET nanobox
    POST_BUILD
    COMMAND ${UTIL_STRIP} ${Target} ${To} 
)