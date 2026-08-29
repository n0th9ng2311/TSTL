# TSTL(Thread Safe Template Library)

---

## Table of contents
- [Overview](#overview)
- [Key features](#key-features)
- [Integration](#integration)
- [AI Disclosure](#ai-disclosure)

---

## Overview
A library that provides thread safe read and write data structures for high performance and everyday use.\
Although
There are several mature and battle tested libraries like [`Boost.Lockfree`](https://www.boost.org/doc/libs/latest/doc/html/lockfree.html)
and [`folly`](https://github.com/facebook/folly) that provide lock free queues and a few other data structures, TSTL aims to provide a 
complete suite of datastructures (and hopefully in future algorithms) which can simply be plug-and-play in any project that may need
concurrent data structures.

TSTL is also designed to feel as familiar as the standard library, here is a simple example using the lockfree `SPSC` class provided in the
library
```cpp
#include <iostream>
#include <thread>
#include <tstl/Queues/SPSC> 

int main() {
    // Create a lock-free SPSC queue for integers. 
    // Capacity must be a power of 2.
    tstl::lockfree::SPSC<int, 1024> queue;

    // Producer Thread
    std::thread producer([&]() {
        for (int i = 1; i <= 5; ++i) {
            // try_emplace returns false if the queue is full
            while (!queue.try_emplace(i)) {
                std::this_thread::yield(); 
            }
            std::cout << "Produced: " << i << '\n';
        }
    });

    // Consumer Thread
    std::thread consumer([&]() {
        for (int i = 1; i <= 5; ++i) {
            std::optional<int> value;
            // try_pop returns std::nullopt if the queue is empty
            while (!(value = queue.try_pop())) {
                std::this_thread::yield();
            }
            std::cout << "Consumed: " << *value << '\n';
        }
    });

    producer.join();
    consumer.join();

    return 0;
}
```
The library is divided into two sections:
- `locking`: Which contains all the datastructures that internally use a mutex for concurrent operations
- `lockfree`: Which contains all the datastructures that do not use a mutex for concurrent operations, these 
structures rely on atomics and provide strong memory ordering guarantees.\
``(Note: Detailed performance graphs and latency metrics can be found in the Benchmarks directory).``

The library is also complete with benchmarks and tests for all available data structures


---

## Key Features
* **Lock-Free Structures:** Guarantees forward progress for threads without relying on heavy mutexes.
* **Custom Slab Allocators:** Replaces standard allocation with slab allocators to reduce memory fragmentation and improve allocation speed.
(although support for this is minimal)
* **Data-Oriented Design:** Memory layouts optimized for hardware cache lines to prevent false sharing in concurrent environments.
* **Modern C++:** Built utilizing strict C++ memory models and atomics.

---

## Integration

### Requirements
- C++ 20 compliant compiler
- Cmake 3.24 or higher

### Integration via CMake FetchContent

You can include TSTL directly in your project using CMake's `FetchContent` module. Add the following to your `CMakeLists.txt`:

```cmake
include(FetchContent)

FetchContent_Declare(
    tstl
    GIT_REPOSITORY https://github.com/n0th9ng2311/TSTL 
    GIT_TAG        v0.1.0 
)
FetchContent_MakeAvailable(tstl)

add_executable(my_app main.cpp)

target_link_libraries(my_app PRIVATE TSTL::tstl)
```

### Integration via vcpkg

Bash
```shell
vcpkg install tstl
```
Once installed integrate it into Cmake as
```cmake
find_package(TSTL_P CONFIG REQUIRED)
target_link_libraries(main PRIVATE TSTL::tstl)
```

---

## License
This project is licensed under the ``MIT License``


*Disclaimer: TSTL is a personal engineering project. I designed it to be fast and safe, 
but it's still a work in progress. Use it at your own risk, and definitely write tests for 
your specific use cases!*

---

## AI Disclosure
Please note that LLM has been used to help draft the documentation although all of that is reviewed by me
