cmake_minimum_required(VERSION 3.20)
project(kalshi_cpp LANGUAGES CXX)

# ── C++ standard ──
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Release flags (applied only with -DCMAKE_BUILD_TYPE=Release) ──
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -flto -fno-exceptions -fno-rtti")

# ── MSYS2 paths (help CMake find libraries installed via pacman) ──
if(MINGW)
    set(MSYS2_PREFIX "C:/Program Files/msys2/ucrt64")
    list(APPEND CMAKE_PREFIX_PATH ${MSYS2_PREFIX})
    set(OPENSSL_ROOT_DIR ${MSYS2_PREFIX})
endif()

# ── Find system libraries ──
find_package(OpenSSL REQUIRED)
# find_package(Boost REQUIRED COMPONENTS system)  # install first: pacman -S mingw-w64-ucrt-x86_64-boost

# ── Fetch third-party libraries ──
include(FetchContent)

# FetchContent_Declare(simdjson
#     GIT_REPOSITORY https://github.com/simdjson/simdjson.git
#     GIT_TAG v3.12.2
# )
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.5
)

set(BENCHMARK_ENABLE_TESTING OFF)
FetchContent_MakeAvailable(googletest googlebenchmark)

# ── Main executable (uncomment as files are implemented) ──
# add_executable(kalshi-cpp
#     src/main.cpp
#     src/net/auth.cpp
#     src/net/ws_client.cpp
#     src/net/rest_client.cpp
#     src/net/ws_reconnect.cpp
#     src/feed/parser.cpp
#     src/feed/book.cpp
#     src/strategy/signal.cpp
#     src/strategy/order_manager.cpp
#     src/system/tuning.cpp
# )
# target_include_directories(kalshi-cpp PRIVATE ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(kalshi-cpp PRIVATE
#     OpenSSL::SSL
#     OpenSSL::Crypto
#     Boost::system
#     Boost::headers
#     simdjson::simdjson
# )

# ── Tests ──
enable_testing()

add_executable(test_spsc test/test_spsc.cpp)
target_include_directories(test_spsc PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_spsc PRIVATE GTest::gtest_main)
add_test(NAME test_spsc COMMAND test_spsc)

add_executable(test_pool test/test_pool.cpp)
target_include_directories(test_pool PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_pool PRIVATE GTest::gtest_main)
add_test(NAME test_pool COMMAND test_pool)

add_executable(test_flat_hash_map test/test_flat_hash_map.cpp)
target_include_directories(test_flat_hash_map PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_flat_hash_map PRIVATE GTest::gtest_main)
add_test(NAME test_flat_hash_map COMMAND test_flat_hash_map)

add_executable(test_rest_client
    test/test_rest_client.cpp
    src/net/rest_client.cpp
)
target_include_directories(test_rest_client PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ── Benchmarks ──
add_executable(bench_spsc bench/bench_spsc.cpp)
target_include_directories(bench_spsc PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bench_spsc PRIVATE benchmark::benchmark_main)

add_executable(bench_pool bench/bench_pool.cpp)
target_include_directories(bench_pool PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bench_pool PRIVATE benchmark::benchmark_main)

add_executable(bench_flat_hash_map bench/bench_flat_hash_map.cpp)
target_include_directories(bench_flat_hash_map PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bench_flat_hash_map PRIVATE benchmark::benchmark_main)

add_executable(test_matching_engine
    test/test_matching_engine.cpp
    sim/matching_engine.cpp
)
target_include_directories(test_matching_engine PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_matching_engine PRIVATE GTest::gtest_main)
add_test(NAME test_matching_engine COMMAND test_matching_engine)

# ── Exchange simulator (kalshi-sim) ──
add_executable(kalshi-sim
    sim/main.cpp
    sim/rest_server.cpp
)
target_include_directories(kalshi-sim PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)