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

function(tstl_test target source)
    add_executable(${target} ${source})
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE TSTL_P gtest_main)
    gtest_discover_tests(${target})
endfunction()

# SPSC
tstl_test(run_spsc_tests queue_tests/SPSC_tests.cpp)

# MPSC
tstl_test(run_mpsc_tests queue_tests/MPSC_tests.cpp)

# Locking queues
tstl_test(run_locking_queue_tests queue_tests/locking_queue_tests.cpp)