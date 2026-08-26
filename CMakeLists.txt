cmake_minimum_required(VERSION 3.24)
project(TSTL_P LANGUAGES CXX)

if (WIN32 AND MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


file(TO_CMAKE_PATH "$ENV{MY_UTILS}" UTILS_DIR)
if (EXISTS "${UTILS_DIR}/init.cmake")
    include("${UTILS_DIR}/init.cmake")
elseif (EXISTS "/init.cmake")
    include("/init.cmake" OPTIONAL)
endif ()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "ccache found: ${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
else()
    message(STATUS "ccache not found. Building without compiler cache.")
endif()

add_library(TSTL_P INTERFACE)

target_include_directories(TSTL_P INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)


add_library(TSTL::tstl ALIAS TSTL_P)

option(TSTL_BUILD_TESTING "Build the test suite" ON)
if (TSTL_BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif ()

option(TSTL_BUILD_BENCHMARKS "Build the benchmark suite" ON)
if (TSTL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS TSTL_P
        EXPORT TSTL_PTargets
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT TSTL_PTargets
        FILE TSTL_PTargets.cmake
        NAMESPACE TSTL::
        DESTINATION share/TSTL_P
)
