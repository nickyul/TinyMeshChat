find_package(unofficial-sodium CONFIG REQUIRED)
add_library(tmc_security STATIC src/security.cpp)
target_include_directories(tmc_security PUBLIC include)
target_link_libraries(tmc_security PUBLIC Qt6::Core PRIVATE unofficial-sodium::sodium)
