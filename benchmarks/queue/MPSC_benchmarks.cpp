#define ANKERL_NANOBENCH_IMPLEMENT
#include <cassert>
#include <nanobench.h>

#include "../../Queues/MPSC.h"
#include "../../common/common_headers.h"

#define BENCH_DO_NOT_OPTIMIZE(expr) ankerl::nanobench::doNotOptimizeAway(expr)

static void pin_thread(int core) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}

// Same-thread round-trip benchmarks
// (raw atomic + construct/destroy overhead; no cross-core contention)

static void bench_same_thread(ankerl::nanobench::Bench &bench) {
    bench.title("Same-Thread Round-Trip (lower bound, no contention)");

    {
        tstl::lockfree::MPSC<int, 1024> queue;
        int val = 42;
        bench.run("MPSC Path B | same-thread | int   ", [&]() {
            queue.emplace(val);
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }
    {
        tstl::lockfree::MPSC<int, 1024> queue;
        int val = 42;
        bench.run("MPSC Path A | same-thread | int   ", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }

    {
        tstl::lockfree::MPSC<double, 1024> queue;
        double val = 3.14;
        bench.run("MPSC Path B | same-thread | double", [&]() {
            queue.emplace(val);
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }
    {
        tstl::lockfree::MPSC<double, 1024> queue;
        double val = 3.14;
        bench.run("MPSC Path A | same-thread | double", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }

    {
        tstl::lockfree::MPSC<std::string, 1024> queue;
        std::string val = "hello";
        bench.run("MPSC Path B | same-thread | string", [&]() {
            queue.emplace(val);
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }
    {
        tstl::lockfree::MPSC<std::string, 1024> queue;
        std::string val = "hello";
        bench.run("MPSC Path A | same-thread | string", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }
}

// Multi-thread throughput benchmarks
// (wall-clock throughput across N producers and 1 consumer)
//
// Returns ns/item (wall-clock elapsed / total items),
// Also asserts that every enqueued item was received: a bug that silently drops
// items would otherwise produce misleadingly fast numbers.

template<typename T, typename Queue>
static double run_multi_thread(Queue &queue, std::size_t items_per_prod, int num_producers, bool use_path_b) {
    const std::size_t total_items = items_per_prod * static_cast<std::size_t>(num_producers);

    auto t_start = std::chrono::steady_clock::now();

    std::vector<std::thread> producers;
    producers.reserve(num_producers);

    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&queue, items_per_prod, p, use_path_b]() {
            pin_thread(p + 1);
            for (std::size_t i{0}; i < items_per_prod; ++i) {
                T val{};
                if constexpr (std::is_arithmetic_v<T>)
                    val = static_cast<T>(i);
                else if constexpr (std::is_same_v<T, std::string>)
                    val = std::to_string(i);

                if (use_path_b) {
                    queue.emplace(val);
                } else {
                    while (!queue.try_emplace(val)) {
                        detail::spin_hint();
                    }
                }
            }
        });
    }

    std::size_t received{0};
    std::thread consumer([&]() {
        pin_thread(0);
        while (received < total_items) {
            if (queue.try_pop().has_value()) {
                ++received;
            } else {
                detail::spin_hint();
            }
        }
    });

    for (auto &t: producers)
        t.join();
    consumer.join();

    auto t_end = std::chrono::steady_clock::now();

    assert(received == total_items && "Consumer did not receive all items — possible MPSC bug");

    double elapsed_ns =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());

    return elapsed_ns / static_cast<double>(total_items);
}

static void print_epoch_stats(const char *label, const std::vector<double> &results) {
    const int n = static_cast<int>(results.size());
    double sum = 0, mn = results[0], mx = results[0];
    for (double v: results) {
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    double mean = sum / n;
    double var = 0;
    for (double v: results)
        var += (v - mean) * (v - mean);
    double err_pct = (n > 1) ? (std::sqrt(var / (n - 1)) / mean * 100.0) : 0.0;

    std::printf("  %-44s  %8.2f ns/item  %8.0f items/s  err%%=%.1f%%  [min=%.1f max=%.1f]\n", label, mean, 1e9 / mean,
                err_pct, mn, mx);
}


template<typename T>
static void run_producer_sweep(const char *type_label, bool use_path_b, std::size_t items_per_prod, int epochs) {
    const char *path_label = use_path_b ? "Path B (fetch_add)" : "Path A (CAS loop)";

    for (int num_producers: {1, 2, 4, 8}) {
        char label[80];
        std::snprintf(label, sizeof(label), "MPSC %s | %s | %dp", path_label, type_label, num_producers);

        std::vector<double> results;
        results.reserve(epochs);
        for (int e = 0; e < epochs; ++e) {
            tstl::lockfree::MPSC<T, 65536> queue;
            results.push_back(run_multi_thread<T>(queue, items_per_prod, num_producers, use_path_b));
        }
        print_epoch_stats(label, results);
    }
}

static void bench_multi_thread() {

    constexpr std::size_t ITEMS_PER_PROD = 1'250'000;
    constexpr int EPOCHS = 7;

    std::puts("  (ns/item = wall-clock elapsed / total items; lower is better)");

    std::puts("\n--- int ---");
    run_producer_sweep<int>("int   ", true, ITEMS_PER_PROD, EPOCHS);
    run_producer_sweep<int>("int   ", false, ITEMS_PER_PROD, EPOCHS);

    std::puts("\n--- double ---");
    run_producer_sweep<double>("double", true, ITEMS_PER_PROD, EPOCHS);
    run_producer_sweep<double>("double", false, ITEMS_PER_PROD, EPOCHS);

    std::puts("\n--- string ---");
    run_producer_sweep<std::string>("string", true, ITEMS_PER_PROD, EPOCHS);
    run_producer_sweep<std::string>("string", false, ITEMS_PER_PROD, EPOCHS);
}

int main() {
    ankerl::nanobench::Bench bench;
    bench.performanceCounters(true);
    bench.minEpochIterations(1085435);

    std::puts("================================================================");
    std::puts(" SECTION 1: Same-Thread Round-Trip");
    std::puts(" Lower bound — no cross-core cache contention");
    std::puts("================================================================\n");
    bench_same_thread(bench);

    std::puts("\n\n");
    std::puts("================================================================");
    std::puts(" SECTION 2: Multi-Thread Throughput Scaling");
    std::puts(" 1 Consumer (Core 0), N Producers (Cores 1-N)");
    std::puts(" Producer counts: 1, 2, 4, 8");
    std::puts("================================================================\n");
    bench_multi_thread();

    return 0;
}
