cmake_minimum_required(VERSION 4.1)

file(TO_CMAKE_PATH "{MY_UTILS}" UTILS_DIR)
include("/init.cmake" OPTIONAL)

include(FetchContent)

FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)

# For Windows compatibility with MSVC runtimes
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(run_unit_tests queue_tests/SPSC_tests.cpp)

target_link_libraries(run_unit_tests
        PRIVATE
        TSTL_P
        gtest_main
)

include(GoogleTest)
gtest_discover_tests(run_unit_tests)