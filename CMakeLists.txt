cmake_minimum_required(VERSION 3.20)
project(TraderTwo LANGUAGES CXX VERSION 0.1.0)

# ---------------------------------------------------------------------------
# Global build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Core static library: header-only style consumers get the include dir
# ---------------------------------------------------------------------------
add_library(tt_core
    src/common/types.cpp
    src/core/order.cpp
    src/orderbook/order_book.cpp
)

target_include_directories(tt_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(tt_core PUBLIC cxx_std_20)

target_compile_options(tt_core
    PUBLIC
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wsign-conversion
        -Wno-unused-parameter
)

# Optimisation flags tuned for the hot path.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(tt_core PUBLIC -O3 -DNDEBUG)
endif()

# ---------------------------------------------------------------------------
# Demo executable
# ---------------------------------------------------------------------------
add_executable(tt_demo examples/demo_phase1.cpp)
target_link_libraries(tt_demo PRIVATE tt_core)

# ---------------------------------------------------------------------------
# Tests (GoogleTest via FetchContent)
# ---------------------------------------------------------------------------
option(TT_BUILD_TESTS "Build TraderTwo unit tests" ON)

if(TT_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(tt_tests
        tests/test_order_book.cpp
        tests/test_common.cpp
    )
    target_link_libraries(tt_tests PRIVATE tt_core GTest::gtest GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(tt_tests)
endif()
