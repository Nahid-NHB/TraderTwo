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
    src/matching/matching_engine.cpp
    src/market_data/publisher.cpp
    src/persistence/event_log.cpp
    src/risk/risk.cpp
    src/concurrency/worker_pool.cpp
    src/networking/protocol.cpp
    src/networking/gateway.cpp
    src/networking/ws_server.cpp
    src/networking/ws_gateway.cpp
    src/observability/metrics.cpp
    src/observability/structured_log.cpp
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

find_package(Threads REQUIRED)
target_link_libraries(tt_core PUBLIC Threads::Threads)

find_package(OpenSSL REQUIRED)
target_link_libraries(tt_core PUBLIC OpenSSL::Crypto)

# ---------------------------------------------------------------------------
# Demo executable
# ---------------------------------------------------------------------------
add_executable(tt_demo examples/demo_phase1.cpp)
target_link_libraries(tt_demo PRIVATE tt_core)

add_executable(tt_simulator examples/simulator.cpp)
target_link_libraries(tt_simulator PRIVATE tt_core)

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
        tests/test_matching_engine.cpp
        tests/test_modify.cpp
        tests/test_instrument_registry.cpp
        tests/test_market_data.cpp
        tests/test_event_log.cpp
        tests/test_risk.cpp
        tests/test_concurrency.cpp
        tests/test_networking.cpp
        tests/test_observability.cpp
        tests/test_stress.cpp
        tests/test_websocket.cpp
    )
    target_link_libraries(tt_tests PRIVATE tt_core GTest::gtest GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(tt_tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks (Google-Benchmark via FetchContent)
# ---------------------------------------------------------------------------
option(TT_BUILD_BENCHMARKS "Build TraderTwo benchmark binaries" ON)

if(TT_BUILD_BENCHMARKS)
    include(FetchContent)
    FetchContent_Declare(
        google_benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(google_benchmark)

    add_executable(tt_benchmarks benchmarks/bench_phase1.cpp)
    target_link_libraries(tt_benchmarks PRIVATE tt_core benchmark::benchmark)
endif()
