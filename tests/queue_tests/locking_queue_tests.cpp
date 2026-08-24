#include <atomic>
#include <barrier>
#include <gtest/gtest.h>
#include <numeric>
#include <thread>
#include <vector>
#include "../../Queues/locking_MPMC.h"
#include "../../Queues/locking_MPSC.h"
#include "../../Queues/locking_SPSC.h"
#include "../../common/common_headers.h"

// TestBundle — same as the SPSC test file
template<typename ConcreteQueue, typename Element>
struct TestBundle {
    using FabricatedQueue = ConcreteQueue;
    using ElementType = Element;
};

template<typename T>
T GeneratePayload(std::size_t index) {
    static_assert(std::is_arithmetic_v<T> || std::is_same_v<T, std::string>);
    if constexpr (std::is_same_v<T, std::string>) {
        return "payload_" + std::to_string(index);
    } else {
        return static_cast<T>(index);
    }
}


//  SUITE 1 — Basic pattern (shared by all three queue types)
//  producer/consumer multiplicity:
//    - empty queue returns nullopt
//    - push + pop round-trips correctly
//    - capacity boundary is respected
//    - ring wraparound preserves FIFO order
template<typename Bundle>
class Locking_basic_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(Locking_basic_pattern);

TYPED_TEST_P(Locking_basic_pattern, EmptyStateTest) { EXPECT_FALSE(this->queue.try_pop().has_value()); }

TYPED_TEST_P(Locking_basic_pattern, SinglePushPopTest) {
    using T = typename TypeParam::ElementType;

    auto val = GeneratePayload<T>(1);
    EXPECT_TRUE(this->queue.try_emplace(val));

    auto result = this->queue.try_pop();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), val);
    EXPECT_FALSE(this->queue.try_pop().has_value());
}

TYPED_TEST_P(Locking_basic_pattern, FullBoundaryTest) {
    using T = typename TypeParam::ElementType;

    for (std::size_t i{0}; i < this->queue.capacity(); ++i) {
        EXPECT_TRUE(this->queue.try_emplace(T{}));
    }
    //should fail
    EXPECT_FALSE(this->queue.try_emplace(T{}));
}

TYPED_TEST_P(Locking_basic_pattern, WrapAroundChasing) {
    using T = typename TypeParam::ElementType;

    const std::size_t capacity = this->queue.capacity();
    const std::size_t window_size = capacity / 2;
    const std::size_t total_iters = capacity * 3;

    // Pre-fill half the queue
    for (std::size_t i{0}; i < window_size; ++i) {
        EXPECT_TRUE(this->queue.try_emplace(GeneratePayload<T>(i)));
    }

    for (std::size_t i{window_size}; i < total_iters; ++i) {
        EXPECT_TRUE(this->queue.try_emplace(GeneratePayload<T>(i))) << "Emplace failed at index " << i;
        auto result = this->queue.try_pop();
        EXPECT_TRUE(result.has_value()) << "Pop failed at index " << i;
        EXPECT_EQ(result.value(), GeneratePayload<T>(i - window_size)) << "Data corrupted at index " << i;
    }

    // Drain the remaining window
    for (std::size_t i{total_iters - window_size}; i < total_iters; ++i) {
        auto result = this->queue.try_pop();
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), GeneratePayload<T>(i));
    }
    EXPECT_FALSE(this->queue.try_pop().has_value());
}

TYPED_TEST_P(Locking_basic_pattern, SizeApproxTest) {
    using T = typename TypeParam::ElementType;

    EXPECT_EQ(this->queue.size_approx(), 0u);
    for (std::size_t i{0}; i < 4; ++i) {
        this->queue.try_emplace(GeneratePayload<T>(i));
    }
    EXPECT_EQ(this->queue.size_approx(), 4u);
    this->queue.try_pop();
    EXPECT_EQ(this->queue.size_approx(), 3u);
}

REGISTER_TYPED_TEST_SUITE_P(Locking_basic_pattern, EmptyStateTest, SinglePushPopTest, FullBoundaryTest,
                            WrapAroundChasing, SizeApproxTest);


//  SUITE 2 — Move-only types (shared by all three queue types)
//
//  Verifies that unique_ptr payloads transfer ownership correctly and that
//  try_emplace accepts rvalue references.
template<typename Bundle>
class Locking_move_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(Locking_move_pattern);

TYPED_TEST_P(Locking_move_pattern, UniqPtrEmplaceTest) {
    using T = typename TypeParam::ElementType;

    T test_val;
    if constexpr (std::is_arithmetic_v<T>) {
        test_val = 40;
    } else if constexpr (std::is_same_v<T, std::string>) {
        test_val = "40";
    }

    auto ptr = std::make_unique<T>(test_val);
    EXPECT_TRUE(this->queue.try_emplace(std::move(ptr)));
    EXPECT_EQ(ptr, nullptr);

    auto result = this->queue.try_pop();
    ASSERT_TRUE(result.has_value());
    ASSERT_NE(result.value(), nullptr);
    EXPECT_EQ(*result.value(), test_val);
}

REGISTER_TYPED_TEST_SUITE_P(Locking_move_pattern, UniqPtrEmplaceTest);


//  SUITE 3 — Blocking API pattern (shared by all three queue types)
//
//  Tests emplace() / pop() — the condvar-backed blocking variants.
//  A separate thread fills / drains while the main thread blocks.
template<typename Bundle>
class Locking_blocking_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(Locking_blocking_pattern);

// Consumer blocks in pop() while the producer hasn't pushed yet, then
// unblocks as soon as the item arrives.
TYPED_TEST_P(Locking_blocking_pattern, BlockingPopUnblocksOnPush) {
    using T = typename TypeParam::ElementType;

    auto expected = GeneratePayload<T>(99);
    T received{};

    std::thread consumer([&] {
        received = this->queue.pop(); // must block until producer pushes
    });

    // Give the consumer a moment to enter the wait
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    this->queue.emplace(expected);
    consumer.join();

    EXPECT_EQ(received, expected);
}

// Producer blocks in emplace() while the queue is full, then
// unblocks as soon as the consumer pops a slot.
TYPED_TEST_P(Locking_blocking_pattern, BlockingEmplaceUnblocksOnPop) {
    using T = typename TypeParam::ElementType;

    // Fill to capacity with try_emplace so emplace() will definitely block
    for (std::size_t i{0}; i < this->queue.capacity(); ++i) {
        ASSERT_TRUE(this->queue.try_emplace(GeneratePayload<T>(i)));
    }

    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        this->queue.emplace(GeneratePayload<T>(999)); // must block until slot freed
        pushed.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(pushed.load(std::memory_order_acquire)); // still blocked

    this->queue.try_pop(); // free one slot
    producer.join();

    EXPECT_TRUE(pushed.load(std::memory_order_acquire));
}

// Round-trip through the blocking API: one producer thread, one consumer thread,
// large item count. Verifies no deadlock and correct sequencing.
TYPED_TEST_P(Locking_blocking_pattern, BlockingRoundTripTest) {
    using T = typename TypeParam::ElementType;
    constexpr std::size_t ITEMS{50'000};

    std::thread producer([&] {
        for (std::size_t i{0}; i < ITEMS; ++i) {
            this->queue.emplace(GeneratePayload<T>(i));
        }
    });

    std::thread consumer([&] {
        for (std::size_t i{0}; i < ITEMS; ++i) {
            T val = this->queue.pop();
            ASSERT_EQ(val, GeneratePayload<T>(i)) << "Mismatch at seq " << i;
        }
    });

    producer.join();
    consumer.join();
}

REGISTER_TYPED_TEST_SUITE_P(Locking_blocking_pattern, BlockingPopUnblocksOnPush, BlockingEmplaceUnblocksOnPop,
                            BlockingRoundTripTest);


//  SUITE 4 — Single-producer / single-consumer concurrency
//
//  Mirrors SeqIntegrityTest from the SPSC suite: one producer, one consumer,
//  verify ordering and completeness under real thread concurrency.
template<typename Bundle>
class Locking_spsc_conc_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(Locking_spsc_conc_pattern);

TYPED_TEST_P(Locking_spsc_conc_pattern, SeqIntegrityTest) {
    using T = typename TypeParam::ElementType;
    constexpr std::size_t ITEMS{100'000};

    std::thread producer([&] {
        for (std::size_t i{0}; i < ITEMS; ++i) {
            while (!this->queue.try_emplace(GeneratePayload<T>(i))) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        for (std::size_t i{0}; i < ITEMS; ++i) {
            std::optional<T> result;
            while (!(result = this->queue.try_pop())) {
                std::this_thread::yield();
            }
            ASSERT_EQ(result.value(), GeneratePayload<T>(i)) << "Failed at seq " << i;
        }
    });

    producer.join();
    consumer.join();
}

REGISTER_TYPED_TEST_SUITE_P(Locking_spsc_conc_pattern, SeqIntegrityTest);


//  SUITE 5 — Multi-producer concurrency  (MPSC / MPMC only)
//  N producers each push a disjoint slice of integers. The single consumer
//  collects all of them, then verifies that every value arrived exactly once
//  (order across producers is non-deterministic and not checked).
template<typename Bundle>
class Locking_mprod_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(Locking_mprod_pattern);

TYPED_TEST_P(Locking_mprod_pattern, MultiProducerAllItemsArriveTest) {
    constexpr std::size_t NUM_PRODUCERS{4};
    constexpr std::size_t ITEMS_PER_PROD{10'000};
    constexpr std::size_t TOTAL{NUM_PRODUCERS * ITEMS_PER_PROD};

    std::atomic<std::size_t> consumed{0};
    std::vector<std::atomic<int>> seen(TOTAL);
    for (auto &a: seen)
        a.store(0, std::memory_order_relaxed);

    // Consumer runs until it has received every item
    std::thread consumer([&] {
        std::size_t count{0};
        while (count < TOTAL) {
            auto result = this->queue.try_pop();
            if (result.has_value()) {
                auto v = static_cast<std::size_t>(result.value());
                seen[v].fetch_add(1, std::memory_order_relaxed);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Each producer pushes its own range: [id*ITEMS_PER_PROD, (id+1)*ITEMS_PER_PROD)
    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);
    for (std::size_t id{0}; id < NUM_PRODUCERS; ++id) {
        producers.emplace_back([&, id] {
            const std::size_t start = id * ITEMS_PER_PROD;
            const std::size_t end = start + ITEMS_PER_PROD;
            for (std::size_t i{start}; i < end; ++i) {
                while (!this->queue.try_emplace(static_cast<int>(i))) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto &p: producers)
        p.join();
    consumer.join();

    // Every value must have been received exactly once
    for (std::size_t i{0}; i < TOTAL; ++i) {
        EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1) << "Value " << i << " received wrong number of times";
    }
}

// Same as above but using the blocking emplace() path
TYPED_TEST_P(Locking_mprod_pattern, MultiProducerBlockingEmplaceTest) {
    constexpr std::size_t NUM_PRODUCERS{4};
    constexpr std::size_t ITEMS_PER_PROD{5'000};
    constexpr std::size_t TOTAL{NUM_PRODUCERS * ITEMS_PER_PROD};

    std::vector<std::atomic<int>> seen(TOTAL);
    for (auto &a: seen)
        a.store(0, std::memory_order_relaxed);

    std::thread consumer([&] {
        for (std::size_t count{0}; count < TOTAL; ++count) {
            std::optional<int> result;
            while (!(result = this->queue.try_pop())) {
                std::this_thread::yield();
            }
            seen[static_cast<std::size_t>(result.value())].fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);
    for (std::size_t id{0}; id < NUM_PRODUCERS; ++id) {
        producers.emplace_back([&, id] {
            const std::size_t start = id * ITEMS_PER_PROD;
            const std::size_t end = start + ITEMS_PER_PROD;
            for (std::size_t i{start}; i < end; ++i) {
                this->queue.emplace(static_cast<int>(i));
            }
        });
    }

    for (auto &p: producers)
        p.join();
    consumer.join();

    for (std::size_t i{0}; i < TOTAL; ++i) {
        EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1) << "Value " << i << " received wrong number of times";
    }
}

REGISTER_TYPED_TEST_SUITE_P(Locking_mprod_pattern, MultiProducerAllItemsArriveTest, MultiProducerBlockingEmplaceTest);


//  SUITE 6 — Multi-consumer concurrency (MPMC only)
//
//  One producer pushes N items; M consumers race to pop them. Verifies that
//  every item is received exactly once across all consumers combined.
template<typename Bundle>
class Locking_mcons_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(Locking_mcons_pattern);

TYPED_TEST_P(Locking_mcons_pattern, MultiConsumerAllItemsReceivedTest) {
    constexpr std::size_t NUM_CONSUMERS{4};
    constexpr std::size_t TOTAL{20'000};

    std::vector<std::atomic<int>> seen(TOTAL);
    for (auto &a: seen)
        a.store(0, std::memory_order_relaxed);
    std::atomic<std::size_t> total_consumed{0};

    std::vector<std::thread> consumers;
    consumers.reserve(NUM_CONSUMERS);
    for (std::size_t id{0}; id < NUM_CONSUMERS; ++id) {
        consumers.emplace_back([&] {
            while (true) {
                auto result = this->queue.try_pop();
                if (result.has_value()) {
                    seen[static_cast<std::size_t>(result.value())].fetch_add(1, std::memory_order_relaxed);
                    if (total_consumed.fetch_add(1, std::memory_order_acq_rel) + 1 >= TOTAL) {
                        break;
                    }
                } else {
                    if (total_consumed.load(std::memory_order_acquire) >= TOTAL)
                        break;
                    std::this_thread::yield();
                }
            }
        });
    }

    std::thread producer([&] {
        for (std::size_t i{0}; i < TOTAL; ++i) {
            while (!this->queue.try_emplace(static_cast<int>(i))) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    for (auto &c: consumers)
        c.join();

    for (std::size_t i{0}; i < TOTAL; ++i) {
        EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1) << "Value " << i << " received wrong number of times";
    }
}

// N producers + M consumers all running simultaneously via blocking API
TYPED_TEST_P(Locking_mcons_pattern, FullMPMCBlockingTest) {
    constexpr std::size_t NUM_PRODUCERS{4};
    constexpr std::size_t NUM_CONSUMERS{4};
    constexpr std::size_t ITEMS_PER_PROD{5'000};
    constexpr std::size_t TOTAL{NUM_PRODUCERS * ITEMS_PER_PROD};

    std::vector<std::atomic<int>> seen(TOTAL);
    for (auto &a: seen)
        a.store(0, std::memory_order_relaxed);
    std::atomic<std::size_t> total_consumed{0};

    std::vector<std::thread> consumers;
    consumers.reserve(NUM_CONSUMERS);
    for (std::size_t id{0}; id < NUM_CONSUMERS; ++id) {
        consumers.emplace_back([&] {
            while (true) {
                // Use blocking pop so consumers sleep rather than spin when empty
                if (total_consumed.load(std::memory_order_acquire) >= TOTAL)
                    break;
                auto result = this->queue.try_pop();
                if (result.has_value()) {
                    seen[static_cast<std::size_t>(result.value())].fetch_add(1, std::memory_order_relaxed);
                    total_consumed.fetch_add(1, std::memory_order_acq_rel);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(NUM_PRODUCERS);
    for (std::size_t id{0}; id < NUM_PRODUCERS; ++id) {
        producers.emplace_back([&, id] {
            const std::size_t start = id * ITEMS_PER_PROD;
            const std::size_t end = start + ITEMS_PER_PROD;
            for (std::size_t i{start}; i < end; ++i) {
                this->queue.emplace(static_cast<int>(i));
            }
        });
    }

    for (auto &p: producers)
        p.join();
    for (auto &c: consumers)
        c.join();

    for (std::size_t i{0}; i < TOTAL; ++i) {
        EXPECT_EQ(seen[i].load(std::memory_order_relaxed), 1) << "Value " << i << " received wrong number of times";
    }
}

REGISTER_TYPED_TEST_SUITE_P(Locking_mcons_pattern, MultiConsumerAllItemsReceivedTest, FullMPMCBlockingTest);


//  SUITE 7 — Destructor drain safety
//
//  Pushes items and destroys the queue without popping. Verifies that the
//  destructor correctly destroys all live objects (no leak, no double-free).
//  Relies on a trivially-tracked type rather than ASAN so it's portable.
template<typename Bundle>
class Locking_dtor_pattern : public ::testing::Test {};

TYPED_TEST_SUITE_P(Locking_dtor_pattern);

TYPED_TEST_P(Locking_dtor_pattern, DestructorDrainsLiveObjects) {
    using T = typename TypeParam::ElementType;
    {
        typename TypeParam::FabricatedQueue q;
        for (std::size_t i{0}; i < q.capacity() / 2; ++i) {
            EXPECT_TRUE(q.try_emplace(GeneratePayload<T>(i)));
        }
    }
    SUCCEED();
}

REGISTER_TYPED_TEST_SUITE_P(Locking_dtor_pattern, DestructorDrainsLiveObjects);


//  TYPE INSTANTIATIONS
// --- locking::Queue (SPSC-style locking) -----------------------------------

using LQ_Basic_Instances = ::testing::Types<TestBundle<tstl::locking::Queue<int, 16>, int>,
                                            TestBundle<tstl::locking::Queue<double, 16>, double>,
                                            TestBundle<tstl::locking::Queue<char, 16>, char>,
                                            TestBundle<tstl::locking::Queue<std::string, 16>, std::string>>;

using LQ_Move_Instances =
        ::testing::Types<TestBundle<tstl::locking::Queue<std::unique_ptr<int>, 16>, int>,
                         TestBundle<tstl::locking::Queue<std::unique_ptr<double>, 16>, double>,
                         TestBundle<tstl::locking::Queue<std::unique_ptr<std::string>, 16>, std::string>>;

using LQ_Blocking_Instances = ::testing::Types<TestBundle<tstl::locking::Queue<int, 64>, int>,
                                               TestBundle<tstl::locking::Queue<std::string, 64>, std::string>>;

using LQ_Conc_Instances = ::testing::Types<TestBundle<tstl::locking::Queue<int, 1024>, int>,
                                           TestBundle<tstl::locking::Queue<double, 1024>, double>,
                                           TestBundle<tstl::locking::Queue<char, 1024>, char>,
                                           TestBundle<tstl::locking::Queue<std::string, 1024>, std::string>>;

using LQ_Dtor_Instances = ::testing::Types<TestBundle<tstl::locking::Queue<std::string, 64>, std::string>>;

INSTANTIATE_TYPED_TEST_SUITE_P(LockingQueue, Locking_basic_pattern, LQ_Basic_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(LockingQueue, Locking_move_pattern, LQ_Move_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(LockingQueue, Locking_blocking_pattern, LQ_Blocking_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(LockingQueue, Locking_spsc_conc_pattern, LQ_Conc_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(LockingQueue, Locking_dtor_pattern, LQ_Dtor_Instances);


// --- locking::MPSC ----------------------------------------------------------

using MPSC_Basic_Instances = ::testing::Types<
        TestBundle<tstl::locking::MPSC<int, 16>, int>, TestBundle<tstl::locking::MPSC<double, 16>, double>,
        TestBundle<tstl::locking::MPSC<char, 16>, char>, TestBundle<tstl::locking::MPSC<std::string, 16>, std::string>>;

using MPSC_Move_Instances =
        ::testing::Types<TestBundle<tstl::locking::MPSC<std::unique_ptr<int>, 16>, int>,
                         TestBundle<tstl::locking::MPSC<std::unique_ptr<double>, 16>, double>,
                         TestBundle<tstl::locking::MPSC<std::unique_ptr<std::string>, 16>, std::string>>;

using MPSC_Blocking_Instances = ::testing::Types<TestBundle<tstl::locking::MPSC<int, 64>, int>,
                                                 TestBundle<tstl::locking::MPSC<std::string, 64>, std::string>>;

using MPSC_Conc_Instances = ::testing::Types<TestBundle<tstl::locking::MPSC<int, 1024>, int>,
                                             TestBundle<tstl::locking::MPSC<double, 1024>, double>,
                                             TestBundle<tstl::locking::MPSC<char, 1024>, char>,
                                             TestBundle<tstl::locking::MPSC<std::string, 1024>, std::string>>;

using MPSC_MProd_Instances = ::testing::Types<TestBundle<tstl::locking::MPSC<int, 1024>, int>>;

using MPSC_Dtor_Instances = ::testing::Types<TestBundle<tstl::locking::MPSC<std::string, 64>, std::string>>;

INSTANTIATE_TYPED_TEST_SUITE_P(MPSC, Locking_basic_pattern, MPSC_Basic_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPSC, Locking_move_pattern, MPSC_Move_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPSC, Locking_blocking_pattern, MPSC_Blocking_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPSC, Locking_spsc_conc_pattern, MPSC_Conc_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPSC, Locking_mprod_pattern, MPSC_MProd_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPSC, Locking_dtor_pattern, MPSC_Dtor_Instances);


// --- locking::MPMC ----------------------------------------------------------

using MPMC_Basic_Instances = ::testing::Types<
        TestBundle<tstl::locking::MPMC<int, 16>, int>, TestBundle<tstl::locking::MPMC<double, 16>, double>,
        TestBundle<tstl::locking::MPMC<char, 16>, char>, TestBundle<tstl::locking::MPMC<std::string, 16>, std::string>>;

using MPMC_Move_Instances =
        ::testing::Types<TestBundle<tstl::locking::MPMC<std::unique_ptr<int>, 16>, int>,
                         TestBundle<tstl::locking::MPMC<std::unique_ptr<double>, 16>, double>,
                         TestBundle<tstl::locking::MPMC<std::unique_ptr<std::string>, 16>, std::string>>;

using MPMC_Blocking_Instances = ::testing::Types<TestBundle<tstl::locking::MPMC<int, 64>, int>,
                                                 TestBundle<tstl::locking::MPMC<std::string, 64>, std::string>>;

using MPMC_Conc_Instances = ::testing::Types<TestBundle<tstl::locking::MPMC<int, 1024>, int>,
                                             TestBundle<tstl::locking::MPMC<double, 1024>, double>,
                                             TestBundle<tstl::locking::MPMC<char, 1024>, char>,
                                             TestBundle<tstl::locking::MPMC<std::string, 1024>, std::string>>;

using MPMC_MProd_Instances = ::testing::Types<TestBundle<tstl::locking::MPMC<int, 1024>, int>>;

using MPMC_MCons_Instances = ::testing::Types<TestBundle<tstl::locking::MPMC<int, 1024>, int>>;

using MPMC_Dtor_Instances = ::testing::Types<TestBundle<tstl::locking::MPMC<std::string, 64>, std::string>>;

INSTANTIATE_TYPED_TEST_SUITE_P(MPMC, Locking_basic_pattern, MPMC_Basic_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPMC, Locking_move_pattern, MPMC_Move_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPMC, Locking_blocking_pattern, MPMC_Blocking_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPMC, Locking_spsc_conc_pattern, MPMC_Conc_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPMC, Locking_mprod_pattern, MPMC_MProd_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPMC, Locking_mcons_pattern, MPMC_MCons_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPMC, Locking_dtor_pattern, MPMC_Dtor_Instances);
