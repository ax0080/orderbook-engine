cmake_minimum_required(VERSION 3.20)
project(OrderbookEngine
    VERSION   1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Release-mode optimizations
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
endif()

option(EXCHANGE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(EXCHANGE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(exchange
    src/order_book.cpp
    src/matching_engine.cpp
)
target_include_directories(exchange PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

option(EXCHANGE_BUILD_EXAMPLES "Build examples" ON)
if(EXCHANGE_BUILD_EXAMPLES)
    add_executable(basic_usage examples/basic_usage.cpp)
    target_link_libraries(basic_usage exchange)
endif()

# ---------------------------------------------------------------------------
# Tests  (GoogleTest, fetched automatically)
# ---------------------------------------------------------------------------
option(EXCHANGE_BUILD_TESTS "Build unit tests" ON)
if(EXCHANGE_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(test_order_book      tests/test_order_book.cpp)
    target_link_libraries(test_order_book      exchange GTest::gtest_main)
    add_test(NAME OrderBookTests      COMMAND test_order_book)

    add_executable(test_matching_engine tests/test_matching_engine.cpp)
    target_link_libraries(test_matching_engine exchange GTest::gtest_main)
    add_test(NAME MatchingEngineTests COMMAND test_matching_engine)

    add_executable(test_fuzz tests/test_fuzz.cpp)
    target_link_libraries(test_fuzz exchange GTest::gtest_main)
    add_test(NAME FuzzTests COMMAND test_fuzz)

    # Differential test against liquibook (header-only, BSD-style license).
    option(EXCHANGE_TEST_AGAINST_LIQUIBOOK "Cross-check fills and depth against liquibook" ON)
    if(EXCHANGE_TEST_AGAINST_LIQUIBOOK)
        FetchContent_Declare(
            liquibook
            GIT_REPOSITORY https://github.com/enewhuis/liquibook.git
            GIT_TAG        2427613b32f1667abae68a01df6af9ba8270f8e7
        )
        FetchContent_MakeAvailable(liquibook)

        add_executable(test_liquibook_diff tests/test_liquibook_diff.cpp)
        target_include_directories(test_liquibook_diff SYSTEM PRIVATE ${liquibook_SOURCE_DIR}/src)
        target_link_libraries(test_liquibook_diff exchange GTest::gtest_main)
        add_test(NAME LiquibookDiffTests COMMAND test_liquibook_diff)
    endif()
endif()

# ---------------------------------------------------------------------------
# Benchmarks  (Google Benchmark, fetched automatically)
# ---------------------------------------------------------------------------
option(EXCHANGE_BUILD_BENCHMARKS "Build micro-benchmarks" ON)
if(EXCHANGE_BUILD_BENCHMARKS)
    include(FetchContent)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.1
    )
    set(BENCHMARK_ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(bench_matching_engine benchmarks/bench_matching_engine.cpp)
    target_link_libraries(bench_matching_engine exchange benchmark::benchmark)

    # Comparison benchmark against liquibook (requires the liquibook fetch from tests)
    if(EXCHANGE_TEST_AGAINST_LIQUIBOOK AND TARGET exchange)
        if(NOT liquibook_POPULATED)
            include(FetchContent)
            FetchContent_Declare(
                liquibook
                GIT_REPOSITORY https://github.com/enewhuis/liquibook.git
                GIT_TAG        2427613b32f1667abae68a01df6af9ba8270f8e7
            )
            FetchContent_MakeAvailable(liquibook)
        endif()
        add_executable(bench_vs_liquibook benchmarks/bench_vs_liquibook.cpp)
        target_include_directories(bench_vs_liquibook SYSTEM PRIVATE ${liquibook_SOURCE_DIR}/src)
        target_link_libraries(bench_vs_liquibook exchange benchmark::benchmark)
    endif()
endif()
