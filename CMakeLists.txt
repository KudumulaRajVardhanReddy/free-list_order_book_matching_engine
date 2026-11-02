# CMakeLists.txt

cmake_minimum_required(VERSION 3.16)
project(LowLevelOrderBook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O3 -Wall -Wextra -Wpedantic -Wno-interference-size -march=native)

include_directories(include)

add_executable(orderbook_app src/main.cpp)
target_link_libraries(orderbook_app PRIVATE pthread)

add_executable(benchmarks_app benchmarks/BenchmarkEngine.cpp)

add_executable(itch_benchmark_app benchmarks/ItchReplayBenchmark.cpp)
target_link_libraries(itch_benchmark_app PRIVATE pthread)
