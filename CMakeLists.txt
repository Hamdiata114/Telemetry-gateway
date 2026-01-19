cmake_minimum_required(VERSION 3.20)
project(telemetry_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core gateway library
add_library(gateway
    src/parse_envelope.cpp
    src/source_limiter.cpp
    src/recv_loop.cpp
)
target_include_directories(gateway PUBLIC include)
target_compile_options(gateway PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

# Test: parse_envelope
add_executable(test_parse_envelope tests/test_parse_envelope.cpp)
target_link_libraries(test_parse_envelope PRIVATE gateway)
add_test(NAME test_parse_envelope COMMAND test_parse_envelope)

# Test: bounded_queue (header-only, no link to gateway needed)
add_executable(test_bounded_queue tests/test_bounded_queue.cpp)
target_include_directories(test_bounded_queue PRIVATE include)
target_compile_options(test_bounded_queue PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME test_bounded_queue COMMAND test_bounded_queue)

# Test: source_limiter
add_executable(test_source_limiter tests/test_source_limiter.cpp)
target_link_libraries(test_source_limiter PRIVATE gateway)
add_test(NAME test_source_limiter COMMAND test_source_limiter)