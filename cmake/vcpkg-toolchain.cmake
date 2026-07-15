set(_tmc_vcpkg_root "$ENV{VCPKG_ROOT}")

if(NOT _tmc_vcpkg_root AND WIN32 AND DEFINED ENV{ProgramFiles})
  set(_tmc_vs_vcpkg "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/VC/vcpkg")
  if(EXISTS "${_tmc_vs_vcpkg}/scripts/buildsystems/vcpkg.cmake")
    set(_tmc_vcpkg_root "${_tmc_vs_vcpkg}")
  endif()
endif()

if(NOT EXISTS "${_tmc_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
  message(FATAL_ERROR
    "vcpkg toolchain was not found. Set VCPKG_ROOT to the vcpkg directory, "
    "then delete the failed CMake cache or select the preset again.")
endif()

include("${_tmc_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
