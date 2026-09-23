vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO paullouisageneau/libdatachannel
    REF "v${VERSION}"
    SHA512 694561ba5b3e08ed35e7e167330d97455ee2ef8d298c9c41e12de4d07032bbe1bb2ebec1e35126187c57ef9492e7d1c82ffd0fa3511eabcdfb77efabfd4b7d9a
    HEAD_REF master
)

# Adapt the upstream dependency names to the packages exported by vcpkg.
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"
    "find_package(plog REQUIRED)" "find_package(plog CONFIG REQUIRED)")
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"
    "find_package(Usrsctp REQUIRED)"
    "find_package(unofficial-usrsctp CONFIG REQUIRED)
add_library(Usrsctp::Usrsctp ALIAS unofficial::usrsctp::usrsctp)")
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt"
    "find_package(libSRTP REQUIRED)" "find_package(libSRTP CONFIG REQUIRED)")

set(TMC_LIBNICE_STATIC OFF)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    set(TMC_LIBNICE_STATIC ON)
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/FindLibNice.cmake.in"
               "${SOURCE_PATH}/cmake/Modules/FindLibNice.cmake" @ONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/LibDataChannelConfig.cmake.in"
               "${SOURCE_PATH}/cmake/LibDataChannelConfig.cmake.in" COPYONLY)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES stdcall CAPI_STDCALL
    INVERTED_FEATURES ws NO_WEBSOCKET srtp NO_MEDIA
)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS ${FEATURE_OPTIONS}
        -DUSE_NICE=ON
        -DPREFER_SYSTEM_LIB=ON
        -DNO_EXAMPLES=ON
        -DNO_TESTS=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/LibDataChannel)
file(INSTALL "${SOURCE_PATH}/cmake/Modules/FindLibNice.cmake"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/rtc/common.hpp" "#ifdef RTC_STATIC" "#if 1")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/rtc/rtc.h" "#ifdef RTC_STATIC" "#if 1")
endif()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
