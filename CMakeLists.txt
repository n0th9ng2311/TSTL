cmake_minimum_required(VERSION 4.1)
project(TSTL_P LANGUAGES CXX)

if(WIN32 AND MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(TO_CMAKE_PATH "{MY_UTILS}" UTILS_DIR)
include("/init.cmake" OPTIONAL)

add_library(TSTL_P STATIC
        allocator/slab_allocator.h
        common/common_headers.h
        Queues/SPSC.h
        common/macros.h
)

target_include_directories(TSTL_P PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

option(BUILD_TESTING "Build the test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

option(BUILD_BENCHMARKS "Build the benchmark suite" ON)
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

set_target_properties(TSTL_P PROPERTIES LINKER_LANGUAGE CXX)
