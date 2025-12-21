cmake_minimum_required(VERSION 3.16)
project(phoenix-discovery VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Library
add_library(pn_discovery STATIC
    src/pn_discovery.c
)

target_include_directories(pn_discovery PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Platform-specific libraries
if(WIN32)
    target_link_libraries(pn_discovery PUBLIC ws2_32 iphlpapi)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(pn_discovery PUBLIC Threads::Threads)
endif()

# Test program
add_executable(test_discovery
    test/test_discovery.c
)
target_link_libraries(test_discovery pn_discovery)

# Install
install(TARGETS pn_discovery
    ARCHIVE DESTINATION lib
)
install(FILES include/pn_discovery.h
    DESTINATION include
)
