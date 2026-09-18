add_subdirectory(init)
add_subdirectory(ttyd)
add_subdirectory(consoled)
add_subdirectory(bin)
add_subdirectory(rust)
add_subdirectory(smoke)
include(${CMAKE_CURRENT_SOURCE_DIR}/busybox-build.cmake)

add_custom_target(make-rfs-symbol ALL
    COMMAND ${UTIL_LN} ${ROOT_FS_DIR}
    DEPENDS kernel nanobox init ttyd consoled busybox naos_rust_smoke
            naos_smoke_suite make-kernel-symbol ${UTIL_LN}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

add_custom_target(update-fake-resource ALL
    COMMAND cp -rf ./run/fakeroot/* ${ROOT_FS_DIR}
    COMMAND ${CMAKE_COMMAND} -E rm -f
            ${ROOT_FS_DIR}/bin/naos-vfsd
            ${ROOT_FS_DIR}/bin/naos-ramdiskd
            ${ROOT_FS_DIR}/bin/naos-exfatd
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/exfatd
            ${ROOT_FS_DIR}/bin/exfatd
    DEPENDS make-rfs-symbol exfatd
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

set(NAOS_ROOT_IMAGE ${CMAKE_BINARY_DIR}/bin/system/root.img)
add_custom_command(
    OUTPUT ${NAOS_ROOT_IMAGE}
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/util/make_root_image.py
            --input ${ROOT_FS_DIR}
            --output ${NAOS_ROOT_IMAGE}
    # `root.img` is the boot bundle's synchronization point.  A targeted
    # build of only one Rust package can refresh the shared Cargo output
    # without copying the other package binaries into bin/system; require all
    # fixed boot modules here so this target can never package a mixed
    # generation of vfsd/ramdiskd/rootfsd/init.
    DEPENDS update-fake-resource
            kernel vfsd ramdiskd rootfsd init_boot_module
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/kernel
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/vfsd
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/ramdiskd
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/rootfsd
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/system/init
            ${PROJECT_SOURCE_DIR}/util/make_root_image.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    VERBATIM)
add_custom_target(make-root-image ALL DEPENDS ${NAOS_ROOT_IMAGE})
