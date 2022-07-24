add_subdirectory(init)
add_subdirectory(bin)

add_custom_target(make-rfs-symbols ALL COMMAND ${UTIL_LN} DEPENDS kernel nanobox init WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_custom_target(make-rfs-image ALL COMMAND python ${UTIL_MKIMG} DEPENDS kernel nanobox init make-rfs-symbols)

