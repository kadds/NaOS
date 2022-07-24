add_subdirectory(init)
add_subdirectory(bin)

add_custom_target(make-rfs-symbol ALL 
    COMMAND ${UTIL_LN} ${ROOT_FS_DIR}
    DEPENDS kernel nanobox init make-kernel-symbol 
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_custom_target(update-fake-resource ALL 
    COMMAND cp -rf ./run/fakeroot/* ${ROOT_FS_DIR} 
    DEPENDS make-rfs-symbol
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_custom_target(make-rfs-image ALL 
    COMMAND python ${UTIL_MKIMG} 
    DEPENDS kernel nanobox init update-fake-resource)
