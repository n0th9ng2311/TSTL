cmake_minimum_required(VERSION 3.24)
project(TSTL_P LANGUAGES CXX)

if(WIN32 AND MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


file(TO_CMAKE_PATH "$ENV{MY_UTILS}" UTILS_DIR)
if(EXISTS "${UTILS_DIR}/init.cmake")
    include("${UTILS_DIR}/init.cmake")
elseif(EXISTS "/init.cmake")
    include("/init.cmake" OPTIONAL)
endif()


add_library(TSTL_P STATIC
        allocator/slab_allocator.h
        common/common_headers.h
        Queues/SPSC.h
        common/macros.h
        Queues/SPSC_A.h
)

target_include_directories(TSTL_P PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

option(TSTL_BUILD_TESTING "Build the test suite" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

option(TSTL_BUILD_BENCHMARKS "Build the benchmark suite" OFF)
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

set_target_properties(TSTL_P PROPERTIES LINKER_LANGUAGE CXX)
