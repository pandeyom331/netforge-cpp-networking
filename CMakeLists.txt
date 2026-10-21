
cmake_minimum_required(VERSION 3.20)

project(NetForge
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Reusable networking protocol library.
add_library(netforge_protocol
    src/protocol.cpp
    src/frame_parser.cpp
)

target_include_directories(netforge_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(netforge_protocol
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)


# TCP server executable.
add_executable(netforge_server
    src/tcp_server.cpp
)

target_link_libraries(netforge_server
    PRIVATE
        netforge_protocol
)

target_compile_options(netforge_server
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

# TCP client executable.
add_executable(netforge_client
    src/tcp_client.cpp
)

target_link_libraries(netforge_client
    PRIVATE
        netforge_protocol
)

target_compile_options(netforge_client
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

# Protocol unit tests.
add_executable(protocol_tests
    tests/protocol_tests.cpp
)

target_link_libraries(protocol_tests
    PRIVATE
        netforge_protocol
)

target_compile_options(protocol_tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

# Frame parser unit tests.
add_executable(frame_parser_tests
    tests/frame_parser_tests.cpp
)

target_link_libraries(frame_parser_tests
    PRIVATE
        netforge_protocol
)

target_compile_options(frame_parser_tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_executable(
    server_integration_tests
    tests/server_integration_tests.cpp
)

target_link_libraries(
    server_integration_tests
    PRIVATE netforge_protocol
)

target_compile_options(
    server_integration_tests
    PRIVATE -Wall -Wextra -Wpedantic
)

add_test(
    NAME server_integration_tests
    COMMAND server_integration_tests $<TARGET_FILE:netforge_server>
)

set_tests_properties(
    server_integration_tests
    PROPERTIES TIMEOUT 20
)

# Register automated tests.
enable_testing()

add_test(
    NAME protocol_tests
    COMMAND protocol_tests
)

add_test(
    NAME frame_parser_tests
    COMMAND frame_parser_tests
)
