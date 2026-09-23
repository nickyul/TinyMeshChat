cmake_minimum_required(VERSION 3.25)

set(package "${TMC_SOURCE_DIR}/dist/TinyMeshSignalingServer-Linux-x64")
set(server "${TMC_BUILD_DIR}/TinyMeshSignalingServer")
if(NOT EXISTS "${server}")
    message(FATAL_ERROR "Build linux-server-release before packaging: ${server}")
endif()
find_program(PATCHELF patchelf REQUIRED)
file(GLOB tls_plugins "${TMC_QT_ROOT}/plugins/tls/*.so")
if(NOT EXISTS "${TMC_QT_ROOT}/plugins/tls/libqopensslbackend.so")
    message(FATAL_ERROR "Qt OpenSSL TLS plugin is required for WSS deployment")
endif()

# Resolve the actual ELF dependency closure, including dependencies of plugins.
# OpenSSL is loaded by Qt at runtime; the deployment OS supplies OpenSSL 3 and
# its CA store. Keep glibc and the ELF loader from the deployment OS as well.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${server}"
    MODULES ${tls_plugins}
    DIRECTORIES "${TMC_QT_ROOT}/lib"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    PRE_EXCLUDE_REGEXES
        "^ld-linux.*" "^lib(c|m|pthread|dl|rt|resolv|util|anl)\\.so\\..*"
)
if(unresolved)
    message(FATAL_ERROR "Unresolved server dependencies: ${unresolved}")
endif()
file(REMOVE_RECURSE "${package}")
file(MAKE_DIRECTORY "${package}/bin" "${package}/lib" "${package}/plugins/tls"
                    "${package}/licenses")
file(COPY "${server}" DESTINATION "${package}/bin")
file(COPY ${tls_plugins} DESTINATION "${package}/plugins/tls")
foreach(dependency IN LISTS dependencies)
    file(COPY "${dependency}" DESTINATION "${package}/lib" FOLLOW_SYMLINK_CHAIN)
endforeach()
file(WRITE "${package}/bin/qt.conf" "[Paths]\nPlugins=../plugins\n")

function(set_rpath path rpath)
    execute_process(COMMAND "${PATCHELF}" --set-rpath "${rpath}" "${path}"
                    COMMAND_ERROR_IS_FATAL ANY)
endfunction()
set_rpath("${package}/bin/TinyMeshSignalingServer" "$ORIGIN/../lib")
file(GLOB bundled_libraries "${package}/lib/*")
foreach(library IN LISTS bundled_libraries)
    if(NOT IS_SYMLINK "${library}")
        set_rpath("${library}" "$ORIGIN")
    endif()
endforeach()
foreach(plugin IN LISTS tls_plugins)
    get_filename_component(name "${plugin}" NAME)
    set_rpath("${package}/plugins/tls/${name}" "$ORIGIN/../../lib")
endforeach()

file(COPY "${TMC_SOURCE_DIR}/LICENSE" DESTINATION "${package}/licenses")
file(GLOB vcpkg_licenses "${TMC_BUILD_DIR}/vcpkg_installed/x64-linux/share/*/copyright")
foreach(license IN LISTS vcpkg_licenses)
    get_filename_component(port_dir "${license}" DIRECTORY)
    get_filename_component(port "${port_dir}" NAME)
    file(INSTALL "${license}" DESTINATION "${package}/licenses/vcpkg/${port}")
endforeach()
if(EXISTS "${TMC_QT_ROOT}/LICENSES")
    file(COPY "${TMC_QT_ROOT}/LICENSES/" DESTINATION "${package}/licenses/Qt")
endif()
file(COPY "${TMC_BUILD_DIR}/tmc-app-version.txt" DESTINATION "${package}")
