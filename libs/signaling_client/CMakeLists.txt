add_library(tmc_signaling_client STATIC)
target_sources(tmc_signaling_client
    PRIVATE
        src/signaling_client.cpp
        src/server_invitation.cpp
        src/access_invitation.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
            include/tmc/signaling_client/signaling_client.h
            include/tmc/signaling_client/server_invitation.h
            include/tmc/signaling_client/access_invitation.h
)
target_link_libraries(tmc_signaling_client
    PUBLIC tmc_security tmc_signaling_protocol Qt6::Core
    PRIVATE Qt6::Network Qt6::WebSockets
)
