vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO paullouisageneau/libplum
    REF 6d2b929c429bc9ea3ecccd2befd3fba60007edbc
    SHA512 a7f904fb95364089aa85932b5be0b4490cc62ed7eb7e99509d1e2991aaca58151a41a600f0decf3304cd045533df64e2a08fe3c17fb8a414dd6c1a9c6be0b17c
    HEAD_REF master
    PATCHES
        namespace-internal-symbols.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DPLUM_NO_EXAMPLE=ON
        -DBUILD_SHARED_LIBS=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME LibPlum CONFIG_PATH lib/cmake/LibPlum)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
