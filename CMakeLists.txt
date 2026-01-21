cmake_minimum_required(VERSION 3.20)
project(telemetry_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core gateway library
add_library(gateway
    src/parse_envelope.cpp
    src/parse_metrics.cpp
    src/parse_log.cpp
    src/validate_config.cpp
    src/validate_metrics.cpp
    src/validate_log.cpp
    src/source_limiter.cpp
    src/recv_loop.cpp
    src/forwarder.cpp
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

# Test: recv_loop (TB-1 enforcement)
add_executable(test_recv_loop tests/test_recv_loop.cpp)
target_link_libraries(test_recv_loop PRIVATE gateway)
add_test(NAME test_recv_loop COMMAND test_recv_loop)

# Test: parse_metrics (TB-3 JSON metrics)
add_executable(test_parse_metrics tests/test_parse_metrics.cpp)
target_link_libraries(test_parse_metrics PRIVATE gateway)
add_test(NAME test_parse_metrics COMMAND test_parse_metrics)

# Test: parse_log (TB-3 logfmt logs)
add_executable(test_parse_log tests/test_parse_log.cpp)
target_link_libraries(test_parse_log PRIVATE gateway)
add_test(NAME test_parse_log COMMAND test_parse_log)

# Test: validate_metrics (TB-4 metrics validation)
add_executable(test_validate_metrics tests/test_validate_metrics.cpp)
target_link_libraries(test_validate_metrics PRIVATE gateway)
add_test(NAME test_validate_metrics COMMAND test_validate_metrics)

# Test: validate_log (TB-4 log validation)
add_executable(test_validate_log tests/test_validate_log.cpp)
target_link_libraries(test_validate_log PRIVATE gateway)
add_test(NAME test_validate_log COMMAND test_validate_log)

# Test: forwarder (TB-5 bounded forwarding with per-agent fairness)
add_executable(test_forwarder tests/test_forwarder.cpp)
target_link_libraries(test_forwarder PRIVATE gateway)
add_test(NAME test_forwarder COMMAND test_forwarder)