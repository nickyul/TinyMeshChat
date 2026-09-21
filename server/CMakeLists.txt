add_executable(TinyMeshSignalingServer)
target_sources(TinyMeshSignalingServer
    PRIVATE
        src/main.cpp
        src/signaling_server.cpp
        src/signaling_server.h
)
target_link_libraries(TinyMeshSignalingServer PRIVATE
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
