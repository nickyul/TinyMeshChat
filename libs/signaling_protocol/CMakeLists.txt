find_package(Protobuf CONFIG REQUIRED)
add_library(tmc_signaling_protocol STATIC)
protobuf_generate(TARGET tmc_signaling_protocol LANGUAGE cpp
    PROTOS "${CMAKE_CURRENT_SOURCE_DIR}/proto/signaling.proto")
target_include_directories(tmc_signaling_protocol PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_sources(tmc_signaling_protocol
    PRIVATE
        src/turn_credentials.cpp
        src/authentication.cpp
        src/envelope_codec.cpp
        src/message_codec.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
            include/tmc/signaling_protocol/turn_credentials.h
            include/tmc/signaling_protocol/authentication.h
            include/tmc/signaling_protocol/envelope.h
            include/tmc/signaling_protocol/envelope_codec.h
            include/tmc/signaling_protocol/message_codec.h
)
target_link_libraries(tmc_signaling_protocol
    PUBLIC Qt6::Core
    PRIVATE tmc_security protobuf::libprotobuf
)
