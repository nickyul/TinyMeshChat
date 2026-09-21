add_library(tmc_signaling_protocol STATIC)
target_sources(tmc_signaling_protocol
    PRIVATE
        src/envelope_codec.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
            include/tmc/signaling_protocol/envelope.h
            include/tmc/signaling_protocol/envelope_codec.h
)
target_link_libraries(tmc_signaling_protocol PUBLIC Qt6::Core)
