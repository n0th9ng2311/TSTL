cmake_minimum_required(VERSION 4.2.2)
project(TSTL_P)

set(CMAKE_CXX_STANDARD 20)

file(TO_CMAKE_PATH "{MY_UTILS}" UTILS_DIR)
include("/init.cmake" OPTIONAL)


add_library(TSTL_P STATIC
        allocator/slab_allocator.h
        "common/common_headers.h"
        Queues/SPSC.h
        common/macros.h)
