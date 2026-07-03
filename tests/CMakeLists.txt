cmake_minimum_required(VERSION 3.24)

project(TSTL_Tests LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

#SPSC tests
add_executable(run_spsc_tests queue_tests/SPSC_tests.cpp)
target_compile_features(run_spsc_tests PRIVATE cxx_std_20)
target_link_libraries(run_spsc_tests
        PRIVATE
        TSTL_P
        gtest_main
)
gtest_discover_tests(run_spsc_tests)

#MPSC tests
add_executable(run_mpsc_tests queue_tests/MPSC_tests.cpp
        queue_tests/MPSC_tests.cpp)
target_compile_features(run_mpsc_tests PRIVATE cxx_std_20)
target_link_libraries(run_mpsc_tests
        PRIVATE
        TSTL_P
        gtest_main
)
gtest_discover_tests(run_mpsc_tests)