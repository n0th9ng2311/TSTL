#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>
#include "../../Queues/SPSC.h"
#include "../../Queues/SPSC_A.h"

#define BENCH_DO_NOT_OPTIMIZE(expr) ankerl::nanobench::doNotOptimizeAway(expr)


static void pin_thread(int core) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}

//  Same-thread round-trip benchmarks
//  (raw atomic + construct/destroy overhead; no cross core contention)

static void bench_same_thread(ankerl::nanobench::Bench &bench) {
    bench.title("Same-Thread Round-Trip (lower bound, no contention)");

    {
        tstl::SPSC<int, 1024> queue;
        int val = 42;
        bench.run("SPSC    | same-thread | int   ", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }

    {
        tstl::SPSC_A<int, 1024> queue;
        int val = 42;
        bench.run("SPSC_A  | same-thread | int   ", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }

    {
        tstl::SPSC<double, 1024> queue;
        double val = 3.14;
        bench.run("SPSC    | same-thread | double", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }

    {
        tstl::SPSC_A<double, 1024> queue;
        double val = 3.14;
        bench.run("SPSC_A  | same-thread | double", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }

    {
        tstl::SPSC<std::string, 1024> queue;
        std::string val = "hello";
        bench.run("SPSC    | same-thread | string", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }

    {
        tstl::SPSC_A<std::string, 1024> queue;
        std::string val = "hello";
        bench.run("SPSC_A  | same-thread | string", [&]() {
            BENCH_DO_NOT_OPTIMIZE(queue.try_emplace(val));
            BENCH_DO_NOT_OPTIMIZE(queue.try_pop());
        });
    }
}

//  Two-thread throughput benchmarks
//  (measures performance with cross-core contention; producer core 0, consumer core 1)

template<typename T, typename Queue>
static double run_two_thread(Queue &queue, std::size_t items) {
    auto t_start = std::chrono::steady_clock::now();

    std::thread producer([&]() {
        pin_thread(0);
        for (std::size_t i{0}; i < items; ++i) {
            T val{};
            if constexpr (std::is_arithmetic_v<T>)
                val = static_cast<T>(i);
            else if constexpr (std::is_same_v<T, std::string>)
                val = std::to_string(i);
            while (!queue.try_emplace(val))
                ;
        }
    });

    std::thread consumer([&]() {
        pin_thread(1);
        std::size_t received{0};
        while (received < items) {
            if (queue.try_pop().has_value())
                ++received;
        }
    });

    producer.join();
    consumer.join();

    auto t_end = std::chrono::steady_clock::now();
    double elapsed_ns =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count());
    return elapsed_ns / static_cast<double>(items); // ns per item
}

static void bench_two_thread(ankerl::nanobench::Bench &bench) {
    bench.title("Two-Thread Throughput (contention, producer=core0, consumer=core1)");

    constexpr std::size_t ITEMS = 5'000'000;

    // using nanobench epochs here but the actual timing is done manually
    //  as nanobench lambda model doesn't compose well with threads.
    //  each epoch re-runs the full producer/consumer pair.
    constexpr int EPOCHS = 7;

    auto run_epochs = [&](auto label, auto queue_factory, auto runner) {
        std::vector<double> results;
        results.reserve(EPOCHS);
        for (int e{0}; e < EPOCHS; ++e) {
            auto queue = queue_factory();
            results.push_back(runner(*queue, ITEMS));
        }

        double sum = 0;
        double mn = results[0];
        double mx = results[0];
        for (double v: results) {
            sum += v;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        double mean = sum / EPOCHS;

        double var = 0;
        for (double v: results) {
            var += (v - mean) * (v - mean);
        }
        double err_pct = (EPOCHS > 1) ? (std::sqrt(var / (EPOCHS - 1)) / mean * 100.0) : 0.0;

        std::printf("  %-42s  %8.2f ns/op  %8.0f op/s  err%%=%.1f%%  [min=%.1f max=%.1f]\n",
            label, mean, 1e9 / mean, err_pct, mn, mx); //std::println still doesnt work well with all the compilers -_-(will update these later)
    };

    std::puts("\n--- int ---");
    run_epochs(
            "SPSC    | two-thread | int   ", [] { return std::make_unique<tstl::SPSC<int, 1024>>(); },
            [](auto &q, std::size_t n) { return run_two_thread<int>(q, n); });
    run_epochs(
            "SPSC_A  | two-thread | int   ", [] { return std::make_unique<tstl::SPSC_A<int, 1024>>(); },
            [](auto &q, std::size_t n) { return run_two_thread<int>(q, n); });

    std::puts("\n--- double ---");
    run_epochs(
            "SPSC    | two-thread | double", [] { return std::make_unique<tstl::SPSC<double, 1024>>(); },
            [](auto &q, std::size_t n) { return run_two_thread<double>(q, n); });
    run_epochs(
            "SPSC_A  | two-thread | double", [] { return std::make_unique<tstl::SPSC_A<double, 1024>>(); },
            [](auto &q, std::size_t n) { return run_two_thread<double>(q, n); });

    std::puts("\n--- string ---");
    run_epochs(
            "SPSC    | two-thread | string", [] { return std::make_unique<tstl::SPSC<std::string, 1024>>(); },
            [](auto &q, std::size_t n) { return run_two_thread<std::string>(q, n); });
    run_epochs(
            "SPSC_A  | two-thread | string", [] { return std::make_unique<tstl::SPSC_A<std::string, 1024>>(); },
            [](auto &q, std::size_t n) { return run_two_thread<std::string>(q, n); });
}

int main() {
    ankerl::nanobench::Bench bench;
    bench.performanceCounters(true);
    bench.minEpochIterations(10854354);

    std::puts(" SECTION 1: Same-Thread Round-Trip");
    std::puts(" Lower bound — no cross-core cache contention");
    std::puts("================================================================\n");
    bench_same_thread(bench);

    std::puts("\n\n");
    std::puts("================================================================");
    std::puts(" SECTION 2: Two-Thread Throughput");
    std::puts(" Producer and consumer on separate cores");
    std::puts("================================================================\n");
    bench_two_thread(bench);

    return 0;
}
