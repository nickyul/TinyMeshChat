add_executable(TinyMeshSignalingServer)
target_sources(TinyMeshSignalingServer
    PRIVATE
        src/main.cpp
        src/turn_service.cpp
        include/tmc/server/turn_service.h
        src/signaling_server.cpp
        include/tmc/server/signaling_server.h
        include/tmc/server/limits.h
        src/room_registry.cpp
        include/tmc/server/room_registry.h
        src/presence_registry.cpp
        include/tmc/server/presence_registry.h
)
target_include_directories(TinyMeshSignalingServer PRIVATE include)
target_link_libraries(TinyMeshSignalingServer PRIVATE
    tmc_security
    tmc_signaling_protocol
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
)
target_compile_definitions(TinyMeshSignalingServer PRIVATE
    TMC_APP_VERSION="${TMC_APP_VERSION}"
)

# Preserve the executable path used by the existing build presets and smoke commands.
set_target_properties(TinyMeshSignalingServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
