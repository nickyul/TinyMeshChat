cmake_minimum_required(VERSION 3.25)

set(TMC_APP_VERSION "0.1.2")
if(NOT TMC_APP_VERSION MATCHES
   "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$")
    message(FATAL_ERROR "TMC_APP_VERSION must be a semantic version")
endif()
set(TMC_VERSION_CORE "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
project(TinyMeshChat VERSION ${TMC_VERSION_CORE} LANGUAGES CXX)
file(WRITE "${CMAKE_BINARY_DIR}/tmc-app-version.txt" "${TMC_APP_VERSION}\n")

option(
    TMC_DEPLOY_QT_AFTER_BUILD
    "Deploy Qt beside the executable after every Windows build"
    ON
)
option(TMC_BUILD_CLIENT "Build the TinyMesh Chat desktop client" ON)
option(TMC_BUILD_SIGNALING_SERVER "Build the optional signaling server" OFF)
option(TMC_ENABLE_UPDATER "Build with Velopack automatic update support" OFF)
set(TMC_VELOPACK_ROOT "" CACHE PATH "Extracted Velopack C/C++ SDK directory")

if(NOT TMC_BUILD_CLIENT AND NOT TMC_BUILD_SIGNALING_SERVER)
    message(FATAL_ERROR "Enable at least one TinyMesh target")
endif()
if(TMC_ENABLE_UPDATER AND NOT TMC_BUILD_CLIENT)
    message(FATAL_ERROR "TMC_ENABLE_UPDATER requires TMC_BUILD_CLIENT")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(DEFINED ENV{QT_ROOT})
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT}")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network WebSockets)

add_subdirectory(libs/security)
add_subdirectory(libs/signaling_protocol)

if(TMC_BUILD_CLIENT)
    add_subdirectory(client)
endif()

if(TMC_BUILD_SIGNALING_SERVER)
    add_subdirectory(server)
endif()
