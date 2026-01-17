cmake_minimum_required(VERSION 3.20)
project(telemetry_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Good hygiene for a security-ish C++ project
add_library(gateway
    src/parse_envelope.cpp
)
target_include_directories(gateway PUBLIC include)
target_compile_options(gateway PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(gateway_tests
    tests/test_parse_envelope.cpp
)
target_link_libraries(gateway_tests PRIVATE gateway)
add_test(NAME gateway_tests COMMAND gateway_tests)