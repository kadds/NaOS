add_subdirectory(init)
add_subdirectory(bin)
include(${CMAKE_CURRENT_SOURCE_DIR}/busybox-build.cmake)

add_custom_target(make-rfs-symbol ALL
    COMMAND ${UTIL_LN} ${ROOT_FS_DIR}
    DEPENDS kernel nanobox init busybox make-kernel-symbol
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_custom_target(update-fake-resource ALL
    COMMAND cp -rf ./run/fakeroot/* ${ROOT_FS_DIR}
    DEPENDS make-rfs-symbol
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_custom_target(make-rfs-image ALL
    COMMAND python3 ${UTIL_MKIMG}
    DEPENDS kernel nanobox init busybox update-fake-resource)
