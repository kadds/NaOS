add_subdirectory(init)
add_subdirectory(bin)

add_custom_target(make-rfs-image ALL COMMAND python ${UTIL_MKIMG} DEPENDS kernel nanobox init make-kernel-symbol)
