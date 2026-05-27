cmake_minimum_required(VERSION 3.20)
project(kalshi_cpp LANGUAGES CXX)

# ── C++ standard ──
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Release flags (applied only with -DCMAKE_BUILD_TYPE=Release) ──
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -fno-exceptions -fno-rtti")

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

# add_executable(test_book test/test_book.cpp)
# target_include_directories(test_book PRIVATE ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(test_book PRIVATE GTest::gtest_main)
# add_test(NAME test_book COMMAND test_book)

# add_executable(test_auth test/test_auth.cpp)
# target_include_directories(test_auth PRIVATE ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(test_auth PRIVATE GTest::gtest_main OpenSSL::SSL OpenSSL::Crypto)
# add_test(NAME test_auth COMMAND test_auth)

# ── Benchmarks ──
add_executable(bench_spsc bench/bench_spsc.cpp)
target_include_directories(bench_spsc PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bench_spsc PRIVATE benchmark::benchmark_main)

# add_executable(bench_allocator bench/bench_allocator.cpp)
# target_include_directories(bench_allocator PRIVATE ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(bench_allocator PRIVATE benchmark::benchmark_main)

# add_executable(bench_parser bench/bench_parser.cpp)
# target_include_directories(bench_parser PRIVATE ${CMAKE_SOURCE_DIR}/src)
# target_link_libraries(bench_parser PRIVATE benchmark::benchmark_main simdjson::simdjson)
