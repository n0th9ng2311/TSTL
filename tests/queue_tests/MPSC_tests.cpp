#include <gtest/gtest.h>
#include "../../Queues/MPSC.h"
#include "../../common/common_headers.h"
#include "../../common/macros.h"

template<typename ConcreteQueue, typename Element>
struct TestBundle {
    using FabricatedQueue = ConcreteQueue;
    using ElementType = Element;
};

template<typename T>
T GeneratePayload(std::size_t index) {
    if constexpr (std::is_same_v<T, std::string>) {
        return "payload_" + std::to_string(index);
    } else {
        return static_cast<T>(index);
    }
}

/// BASIC LOGIC AND WRAPAROUND TESTS \\\

template<typename Bundle>
class MPSC_basic_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(MPSC_basic_pattern);

TYPED_TEST_P(MPSC_basic_pattern, EmptyStateTest) {
    auto result = this->queue.try_pop();
    EXPECT_FALSE(result.has_value());
}

TYPED_TEST_P(MPSC_basic_pattern, SinglePushPopTest) {
    using T = typename TypeParam::ElementType;

    auto val = GeneratePayload<T>(1);
    EXPECT_TRUE(this->queue.try_emplace(val));

    auto result = this->queue.try_pop();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), val);
    EXPECT_FALSE(this->queue.try_pop().has_value());
}

TYPED_TEST_P(MPSC_basic_pattern, FullBoundaryTest) {
    using T = typename TypeParam::ElementType;

    for (std::size_t i{0}; i < this->queue.capacity(); i++) {
        EXPECT_TRUE(this->queue.try_emplace(T()));
    }

    EXPECT_FALSE(this->queue.try_emplace(T()));
}

TYPED_TEST_P(MPSC_basic_pattern, WrapAroundChasing) {
    using T = typename TypeParam::ElementType;

    const std::size_t capacity = this->queue.capacity();
    const std::size_t window_size = capacity / 2;
    const std::size_t total_iterations = capacity * 3;

    for (std::size_t i{0}; i < window_size; ++i) {
        EXPECT_TRUE(this->queue.try_emplace(GeneratePayload<T>(i)));
    }

    for (std::size_t i{window_size}; i < total_iterations; ++i) {
        EXPECT_TRUE(this->queue.try_emplace(GeneratePayload<T>(i))) << "Emplace failed at index " << i;

        auto result = this->queue.try_pop();
        EXPECT_TRUE(result.has_value()) << "Pop failed at index " << i;
        EXPECT_EQ(result.value(), GeneratePayload<T>(i - window_size)) << "Data corrupted at index " << i;
    }

    // Drain the remaining window
    for (std::size_t i{total_iterations - window_size}; i < total_iterations; ++i) {
        auto result = this->queue.try_pop();
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), GeneratePayload<T>(i));
    }

    EXPECT_FALSE(this->queue.try_pop().has_value());
}

REGISTER_TYPED_TEST_SUITE_P(MPSC_basic_pattern, EmptyStateTest, SinglePushPopTest, FullBoundaryTest, WrapAroundChasing);


/// MOVE ONLY TYPES TEST \\\

template<typename Bundle>
class MPSC_move_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(MPSC_move_pattern);

TYPED_TEST_P(MPSC_move_pattern, UniqPtrEmplaceTest) {
    using T = typename TypeParam::ElementType;

    T test_val;
    if constexpr (std::is_arithmetic_v<T>) {
        test_val = 40;
    } else if constexpr (std::is_same_v<T, std::string>) {
        test_val = "40";
    }

    auto ptr = std::make_unique<T>(test_val);

    EXPECT_TRUE(this->queue.try_emplace(std::move(ptr)));
    EXPECT_EQ(ptr, nullptr); //verifying that the owner gave up ownership

    auto result = this->queue.try_pop();
    EXPECT_TRUE(result.has_value());
    EXPECT_NE(result.value(), nullptr);
    EXPECT_EQ(*result.value(), test_val);

    // Path B API test for move semantics
    auto ptr2 = std::make_unique<T>(test_val);
    this->queue.emplace(std::move(ptr2));
    EXPECT_EQ(ptr2, nullptr);

    auto result2 = this->queue.try_pop();
    EXPECT_TRUE(result2.has_value());
}

REGISTER_TYPED_TEST_SUITE_P(MPSC_move_pattern, UniqPtrEmplaceTest);


/// CONCURRENCY (MULTI-PRODUCER) TESTS \\\

template<typename Bundle>
class MPSC_conc_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(MPSC_conc_pattern);

//testing the CAS try_emplace
TYPED_TEST_P(MPSC_conc_pattern, MultiProducer_TryEmplace) {
    using T = typename TypeParam::ElementType;
    constexpr std::size_t NUM_PRODUCERS = 4;
    constexpr std::size_t ITEMS_PER_PROD = 50'000;
    constexpr std::size_t TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PROD;

    std::vector<std::thread> producers;

    for (std::size_t p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&]() {
            for (std::size_t i{0}; i < ITEMS_PER_PROD; i++) {
                auto val = GeneratePayload<T>(i);
                while (!this->queue.try_emplace(std::move(val))) {
                    detail::spin_hint();
                }
            }
        });
    }

    std::size_t consumed = 0;
    while (consumed < TOTAL_ITEMS) {
        if (auto result = this->queue.try_pop()) {
            consumed++;
        } else {
            detail::spin_hint();
        }
    }

    for (auto& prod : producers) {
        prod.join();
    }

    EXPECT_EQ(consumed, TOTAL_ITEMS);
}

//testing the blocking emplace
TYPED_TEST_P(MPSC_conc_pattern, MultiProducer_BlockingEmplace) {
    using T = typename TypeParam::ElementType;
    constexpr std::size_t NUM_PRODUCERS = 4;
    constexpr std::size_t ITEMS_PER_PROD = 50'000;
    constexpr std::size_t TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PROD;

    std::vector<std::thread> producers;

    for (std::size_t p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&]() {
            for (std::size_t i{0}; i < ITEMS_PER_PROD; i++) {
                this->queue.emplace(GeneratePayload<T>(i));
            }
        });
    }

    std::size_t consumed = 0;
    while (consumed < TOTAL_ITEMS) {
        if (auto result = this->queue.try_pop()) {
            consumed++;
        } else {
            detail::spin_hint();
        }
    }

    for (auto& prod : producers) {
        prod.join();
    }

    EXPECT_EQ(consumed, TOTAL_ITEMS);
}

REGISTER_TYPED_TEST_SUITE_P(MPSC_conc_pattern, MultiProducer_TryEmplace, MultiProducer_BlockingEmplace);


/// Test Inst \\\

// Basic Tests
using MPSC_Basic_Instances = ::testing::Types<
    TestBundle<tstl::lockfree::MPSC<int, 16>, int>,
    TestBundle<tstl::lockfree::MPSC<double, 16>, double>,
    TestBundle<tstl::lockfree::MPSC<char, 16>, char>,
    TestBundle<tstl::lockfree::MPSC<std::string, 16>, std::string>
>;

// Move-Only Tests
using MPSC_Move_Instances = ::testing::Types<
    TestBundle<tstl::lockfree::MPSC<std::unique_ptr<int>, 16>, int>,
    TestBundle<tstl::lockfree::MPSC<std::unique_ptr<double>, 16>, double>,
    TestBundle<tstl::lockfree::MPSC<std::unique_ptr<std::string>, 16>, std::string>
>;

// Concurrency Tests (Larger Queue Size to absorb initial thread spikes)
using MPSC_Conc_Instances = ::testing::Types<
    TestBundle<tstl::lockfree::MPSC<int, 1024>, int>,
    TestBundle<tstl::lockfree::MPSC<double, 1024>, double>,
    TestBundle<tstl::lockfree::MPSC<char, 1024>, char>,
    TestBundle<tstl::lockfree::MPSC<std::string, 1024>, std::string>
>;

INSTANTIATE_TYPED_TEST_SUITE_P(MPSC_Queue, MPSC_basic_pattern, MPSC_Basic_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPSC_Queue, MPSC_move_pattern, MPSC_Move_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(MPSC_Queue, MPSC_conc_pattern, MPSC_Conc_Instances);