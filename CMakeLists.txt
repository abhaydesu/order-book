cmake_minimum_required(VERSION 3.20)
project(OrderbookEngine LANGUAGES CXX)

# Standard C++20 for high-performance data structures
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# -DSANITIZE=address (ASan + UBSan) or -DSANITIZE=thread (TSan)
set(SANITIZE "" CACHE STRING "Sanitizer to build with: address or thread")
if(SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
elseif(SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT SANITIZE STREQUAL "")
    message(FATAL_ERROR "SANITIZE must be address or thread")
endif()

find_package(Threads REQUIRED)

# The orderbook itself, shared by the app, tests and benchmark
add_library(orderbook_lib Orderbook.cpp)
target_include_directories(orderbook_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orderbook_lib PUBLIC Threads::Threads)

add_executable(orderbook main.cpp)
target_link_libraries(orderbook PRIVATE orderbook_lib)

add_executable(orderbook_bench bench/benchmark.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook_lib)

# Tests (GoogleTest is downloaded on first configure)
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(orderbook_tests OrderbookTest/test.cpp OrderbookTest/regression_test.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook_lib GTest::gtest_main)

include(GoogleTest)
# The timeout turns a deadlock or a hang on shutdown into a test failure.
gtest_discover_tests(orderbook_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/OrderbookTest
    PROPERTIES TIMEOUT 10)
