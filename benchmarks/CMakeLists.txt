cmake_minimum_required(VERSION 4.1)

file(TO_CMAKE_PATH "{MY_UTILS}" UTILS_DIR)
include("/init.cmake" OPTIONAL)

include(FetchContent)

FetchContent_Declare(
        nanobench
        URL https://github.com/martinus/nanobench/archive/refs/tags/v4.3.11.zip
)
FetchContent_MakeAvailable(nanobench)

add_executable(run_benchmarks queue/SPSC_benchmarks.cpp)

target_link_libraries(run_benchmarks
        PRIVATE
        TSTL_P
        nanobench::nanobench
)