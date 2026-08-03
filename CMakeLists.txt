cmake_minimum_required(VERSION 3.25)
project(TinyMeshChat VERSION 0.1.0 LANGUAGES CXX)

option(
    TMC_DEPLOY_QT_AFTER_BUILD
    "Deploy Qt beside the executable after every Windows build"
    ON
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(DEFINED ENV{QT_ROOT})
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT}")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)
find_path(SPEEXDSP_INCLUDE_DIR speex/speex_preprocess.h REQUIRED)
find_library(SPEEXDSP_LIBRARY NAMES speexdsp REQUIRED)

add_library(tmc_core STATIC)
target_sources(tmc_core
    PRIVATE
        src/core/app_config.cpp
        src/core/logger.cpp
        src/core/uuid.cpp
        src/identity/peer_identity.cpp
        src/identity/identity_manager.cpp
        src/protocol/packet.cpp
        src/protocol/packet_codec.cpp
        src/messaging/chat_message.cpp
        src/messaging/delivery_tracker.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
            include/tmc/core/app_config.h
            include/tmc/core/limits.h
            include/tmc/core/logger.h
            include/tmc/core/result.h
            include/tmc/core/uuid.h
            include/tmc/identity/identity_manager.h
            include/tmc/identity/peer_identity.h
            include/tmc/protocol/packet.h
            include/tmc/protocol/packet_codec.h
            include/tmc/messaging/chat_message.h
            include/tmc/messaging/delivery_tracker.h
)
target_link_libraries(tmc_core PUBLIC Qt6::Core)

add_library(tmc_transport STATIC)
target_sources(tmc_transport
    PRIVATE
        src/network/peer_connection.cpp
        src/network/sdp_description_codec.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
            include/tmc/network/connection_attempt_state.h
            include/tmc/network/connection_policy.h
            include/tmc/network/connection_state.h
            include/tmc/network/peer_connection.h
            include/tmc/network/sdp_description_codec.h
)
target_link_libraries(tmc_transport
    PUBLIC tmc_core Qt6::Core
    PRIVATE
        Qt6::Network
        LibDataChannel::LibDataChannel
)

add_library(tmc_audio STATIC)
target_sources(tmc_audio
    PRIVATE
        src/audio/remote_audio_stream.cpp
        src/audio/rtp_audio_engine.cpp
        include/tmc/audio/audio_network_stats.h
        include/tmc/audio/remote_audio_stream.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
            include/tmc/audio/rtp_audio_engine.h
)
target_include_directories(tmc_audio PRIVATE
    "${MINIAUDIO_INCLUDE_DIR}"
    "${SPEEXDSP_INCLUDE_DIR}"
)
target_link_libraries(tmc_audio
    PUBLIC
        tmc_core
        Qt6::Core
    PRIVATE
        Opus::opus
        "${SPEEXDSP_LIBRARY}"
)
if(APPLE)
    # TinyMesh Chat uses miniaudio's native CoreAudio backend on macOS. Keeping
    # only that backend avoids probing desktop backends that do not apply to a
    # signed .app bundle and makes the selected audio backend deterministic.
    target_compile_definitions(tmc_audio PRIVATE
        MA_ENABLE_ONLY_SPECIFIC_BACKENDS
        MA_ENABLE_COREAUDIO
    )
endif()

add_library(tmc_application STATIC)
target_sources(tmc_application
    PRIVATE
        src/app/application_controller.cpp
        src/app/connection_manager.cpp
        src/app/mesh_coordinator.cpp
        src/app/messaging_service.cpp
        src/app/network_session.cpp
        src/app/network_session_packets.cpp
        src/app/signaling_router.cpp
        src/app/negotiation_policy.cpp
        src/app/voice_session.cpp
        src/signaling/invitation_codec.cpp
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
            include/tmc/app/signaling_router.h
            include/tmc/app/negotiation_policy.h
            include/tmc/app/voice_session.h
            include/tmc/signaling/invitation.h
            include/tmc/signaling/invitation_codec.h
)
target_link_libraries(tmc_application
    PUBLIC
        tmc_core
        tmc_transport
        Qt6::Core
    PRIVATE
        tmc_audio
        Qt6::Network
)

add_executable(TinyMeshChat)
target_sources(TinyMeshChat
    PRIVATE
        src/main.cpp
        src/bootstrap/application_options.cpp
        src/bootstrap/console_runtime.cpp
        src/bootstrap/gui_runtime.cpp
        src/cli/console_controller.cpp
        src/ui/app_link_controller.cpp
        src/ui/app_view_model.cpp
        resources/resources.qrc
    PRIVATE
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
            include/tmc/bootstrap/application_options.h
            include/tmc/bootstrap/console_runtime.h
            include/tmc/bootstrap/gui_runtime.h
            include/tmc/cli/console_controller.h
            include/tmc/ui/app_link_controller.h
            include/tmc/ui/app_view_model.h
)

if(APPLE)
    set_target_properties(TinyMeshChat PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "TinyMesh Chat"
        MACOSX_BUNDLE_GUI_IDENTIFIER "chat.tinymesh.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )
elseif(WIN32)
    set_target_properties(TinyMeshChat PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

target_link_libraries(TinyMeshChat PRIVATE
    tmc_application
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

if(WIN32 AND TMC_DEPLOY_QT_AFTER_BUILD)
    add_custom_command(TARGET TinyMeshChat POST_BUILD
        COMMAND Qt6::windeployqt
            --no-translations
            --compiler-runtime
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
            --dir "$<TARGET_FILE_DIR:TinyMeshChat>"
            "$<TARGET_FILE:TinyMeshChat>"
        COMMENT "Deploying the Qt runtime for direct IDE execution"
        VERBATIM
    )
endif()
