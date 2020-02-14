add_subdirectory(common)
add_subdirectory(init)
add_subdirectory(syscall_test)
add_subdirectory(nsh)

add_custom_target(make-rfs-image ALL COMMAND python ${UTIL_MKIMG} DEPENDS kernel init syscall_test nsh make-kernel-symbol)
