#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>
#include "../../Queues/SPSC.h"

int main() {
    ankerl::nanobench::Bench bench;
    bench.performanceCounters(true);

    bench.minEpochIterations(706532);

    tstl::SPSC_A<int, 1024> queue_alloc;
    tstl::SPSC<int, 1024> queue;
    int val = 42;

    bench.run("SPSC Round-Trip (int)", [&]() {
        ankerl::nanobench::doNotOptimizeAway(queue.try_emplace(val));
        ankerl::nanobench::doNotOptimizeAway(queue.try_pop());
    });

    bench.run("SPSC_A Round-Trip (int)", [&]() {
        ankerl::nanobench::doNotOptimizeAway(queue_alloc.try_emplace(val));
        ankerl::nanobench::doNotOptimizeAway(queue_alloc.try_pop());
    });


    return 0;
}