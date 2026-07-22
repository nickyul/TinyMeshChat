cmake_minimum_required(VERSION 3.25)
project(TinyMeshChat VERSION 0.1.0 LANGUAGES CXX)

option(TMC_BUILD_GUI "Build the Qt Quick application" ON)
option(TMC_WITH_LIBDATACHANNEL "Enable the real WebRTC transport" ON)
option(TMC_DEPLOY_QT_AFTER_BUILD "Deploy Qt beside the executable after every Windows build" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(TMC_BUILD_GUI AND NOT TMC_WITH_LIBDATACHANNEL)
  message(FATAL_ERROR
    "TMC_BUILD_GUI requires TMC_WITH_LIBDATACHANNEL because NetworkSession uses the "
    "WebRTC transport")
endif()

if(NOT Qt6_DIR)
  if(DEFINED ENV{QT_ROOT} AND EXISTS "$ENV{QT_ROOT}")
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT}")
  elseif(WIN32 AND MSVC)
    file(GLOB _tmc_qt_candidates LIST_DIRECTORIES true "C:/Qt/6.*/msvc2022_64")
    list(SORT _tmc_qt_candidates COMPARE NATURAL ORDER DESCENDING)
    if(_tmc_qt_candidates)
      list(GET _tmc_qt_candidates 0 _tmc_qt_root)
      list(APPEND CMAKE_PREFIX_PATH "${_tmc_qt_root}")
      message(STATUS "TinyMesh Chat detected Qt at ${_tmc_qt_root}")
    endif()
  endif()
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core)
if(TMC_BUILD_GUI)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Gui Network Qml Quick QuickControls2)
  find_package(Opus CONFIG REQUIRED)
  find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
  find_path(SPEEXDSP_INCLUDE_DIR speex/speex_preprocess.h REQUIRED)
  find_library(SPEEXDSP_LIBRARY NAMES speexdsp REQUIRED)
endif()
if(TMC_WITH_LIBDATACHANNEL)
  find_package(LibDataChannel CONFIG REQUIRED)
endif()

add_library(tmc_core)
target_sources(tmc_core
  PRIVATE
    src/core/app_config.cpp
    src/core/logger.cpp
    src/identity/peer_identity.cpp
    src/identity/identity_manager.cpp
    src/signaling/invitation.cpp
    src/signaling/invitation_codec.cpp
    src/protocol/packet.cpp
    src/protocol/packet_codec.cpp
    src/protocol/packet_dispatcher.cpp
    src/messaging/chat_message.cpp
    src/messaging/delivery_tracker.cpp
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    FILES
      include/tmc/core/app_config.h
      include/tmc/core/logger.h
      include/tmc/core/result.h
      include/tmc/core/voice_frame_timing.h
      include/tmc/identity/identity_manager.h
      include/tmc/identity/peer_identity.h
      include/tmc/signaling/invitation.h
      include/tmc/signaling/invitation_codec.h
      include/tmc/protocol/packet.h
      include/tmc/protocol/packet_codec.h
      include/tmc/protocol/packet_dispatcher.h
      include/tmc/messaging/chat_message.h
      include/tmc/messaging/delivery_tracker.h)
target_include_directories(tmc_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(tmc_core PUBLIC Qt6::Core)
target_compile_definitions(tmc_core PUBLIC TMC_VERSION="${PROJECT_VERSION}")

if(TMC_WITH_LIBDATACHANNEL)
  add_library(tmc_transport)
  target_sources(tmc_transport
    PRIVATE
      src/network/peer_connection.cpp
    PUBLIC
      FILE_SET HEADERS
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
        include/tmc/network/connection_attempt_state.h
        include/tmc/network/connection_policy.h
        include/tmc/network/connection_state.h
        include/tmc/network/peer_connection.h)
  target_include_directories(tmc_transport PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
  target_link_libraries(tmc_transport
    PUBLIC tmc_core Qt6::Core
    PRIVATE LibDataChannel::LibDataChannel)
  target_compile_definitions(tmc_transport PUBLIC TMC_WITH_LIBDATACHANNEL=1)
endif()

if(TMC_BUILD_GUI)
  add_library(tmc_audio)
  target_sources(tmc_audio
    PRIVATE
      src/audio/remote_audio_stream.cpp
      src/audio/rtp_audio_engine.cpp
    PUBLIC
      FILE_SET HEADERS
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
        include/tmc/audio/audio_network_stats.h
        include/tmc/audio/remote_audio_stream.h
        include/tmc/audio/rtp_audio_engine.h)
  target_include_directories(tmc_audio
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${MINIAUDIO_INCLUDE_DIR}" "${SPEEXDSP_INCLUDE_DIR}")
  target_link_libraries(tmc_audio
    PUBLIC tmc_core Qt6::Core
    PRIVATE Opus::opus "${SPEEXDSP_LIBRARY}")
  if(APPLE)
    # TinyMesh Chat uses miniaudio's native CoreAudio backend on macOS. Keeping
    # only that backend avoids probing desktop backends that do not apply to a
    # signed .app bundle and makes the selected transport deterministic.
    target_compile_definitions(tmc_audio PRIVATE
      MA_ENABLE_ONLY_SPECIFIC_BACKENDS
      MA_ENABLE_COREAUDIO)
  endif()

  add_library(tmc_application)
  target_sources(tmc_application
    PRIVATE
      src/app/application_controller.cpp
      src/app/connection_manager.cpp
      src/app/mesh_coordinator.cpp
      src/app/messaging_service.cpp
      src/app/network_session.cpp
      src/app/peer_registry.cpp
      src/app/session_packet_handlers.cpp
      src/app/signaling_router.cpp
      src/app/topology_controller.cpp
      src/app/voice_session.cpp
      src/cli/console_controller.cpp
    PUBLIC
      FILE_SET HEADERS
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
        include/tmc/app/application_controller.h
        include/tmc/app/connection_manager.h
        include/tmc/app/mesh_session_state.h
        include/tmc/app/mesh_coordinator.h
        include/tmc/app/messaging_service.h
        include/tmc/app/network_session.h
        include/tmc/app/peer_registry.h
        include/tmc/app/session_packet_handlers.h
        include/tmc/app/voice_session.h
        include/tmc/cli/console_controller.h)
  target_include_directories(tmc_application PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
  target_link_libraries(tmc_application
    PUBLIC tmc_core tmc_transport tmc_audio Qt6::Core Qt6::Network)

  set(_tmc_app_sources
    src/main.cpp
    src/ui/app_link_controller.cpp
    src/ui/app_view_model.cpp
    resources/resources.qrc)
  if(APPLE)
    configure_file(resources/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
    add_executable(TinyMeshChat MACOSX_BUNDLE ${_tmc_app_sources})
    set_target_properties(TinyMeshChat PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
      MACOSX_BUNDLE_BUNDLE_NAME "TinyMesh Chat"
      MACOSX_BUNDLE_GUI_IDENTIFIER "chat.tinymesh.app"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
  elseif(WIN32)
    add_executable(TinyMeshChat WIN32 ${_tmc_app_sources})
  else()
    add_executable(TinyMeshChat ${_tmc_app_sources})
  endif()
  target_sources(TinyMeshChat
    PRIVATE
      FILE_SET HEADERS
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
        include/tmc/ui/app_link_controller.h
        include/tmc/ui/app_view_model.h)
  target_include_directories(TinyMeshChat PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(TinyMeshChat PRIVATE
    tmc_application Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2)

  if(WIN32 AND TMC_DEPLOY_QT_AFTER_BUILD)
    get_target_property(_tmc_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_tmc_qt_bin_dir "${_tmc_qmake_executable}" DIRECTORY)
    find_program(_tmc_windeployqt
      NAMES windeployqt6 windeployqt
      HINTS "${_tmc_qt_bin_dir}"
      REQUIRED)

    add_custom_command(TARGET TinyMeshChat POST_BUILD
      COMMAND "${_tmc_windeployqt}"
        --no-translations
        --compiler-runtime
        --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
        --dir "$<TARGET_FILE_DIR:TinyMeshChat>"
        "$<TARGET_FILE:TinyMeshChat>"
      COMMENT "Deploying the Qt runtime for direct IDE execution"
      VERBATIM)
  endif()
endif()

if(TMC_BUILD_GUI)
  install(TARGETS TinyMeshChat BUNDLE DESTINATION . RUNTIME DESTINATION .)
endif()
