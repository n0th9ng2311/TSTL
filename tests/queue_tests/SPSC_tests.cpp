#include <gtest/gtest.h>
#include "../../Queues/SPSC.h"
#include "../../Queues/SPSC_A.h" // Include your second queue variant here
#include "../../common/common_headers.h"

// Helper to handle types across tests
template<typename T>
T GeneratePayload(size_t index) {
    if constexpr (std::is_same_v<T, std::string>) {
        return "payload_" + std::to_string(index);
    } else {
        return static_cast<T>(index);
    }
}

// Standard types list
using TestingTypes = ::testing::Types<int, double, char, std::string>;
// Move-only types list
using MoveTestingTypes = ::testing::Types<int, double, std::string>;


/// ============================================================================
/// 1. PATTERN: BASIC LOGIC & WRAP-AROUND TESTS
/// ============================================================================

template <typename QueueType>
class SPSC_basic_pattern : public ::testing::Test {
protected:
    // QueueType will be bound to something like tstl::SPSC<T, 16>
    QueueType queue;
};

TYPED_TEST_SUITE_P(SPSC_basic_pattern);

TYPED_TEST_P(SPSC_basic_pattern, EmptyStateTest) {
    auto result = this->queue.try_pop();
    EXPECT_FALSE(result.has_value());
}

TYPED_TEST_P(SPSC_basic_pattern, SinglePushPopTest) {
    using TypeParam = typename TypeParamTrait<TypeParam>::Type; // Access the underlying T if needed, or instantiate directly
    auto val = GeneratePayload<typename TypeParamTrait<TypeParam>::Type>(1);
    EXPECT_TRUE(this->queue.try_emplace(val));

    auto result = this->queue.try_pop();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), val);
    EXPECT_FALSE(this->queue.try_pop().has_value());
}

TYPED_TEST_P(SPSC_basic_pattern, FullBoundaryTest) {
    using T = typename TypeParamTrait<TypeParam>::Type;
    for (std::size_t i{0}; i < 16; i++) {
        EXPECT_TRUE(this->queue.try_emplace(T()));
    }
    EXPECT_FALSE(this->queue.try_emplace(T()));
}

TYPED_TEST_P(SPSC_basic_pattern, WrapAroundChasing) {
    using T = typename TypeParamTrait<TypeParam>::Type;
    const size_t capacity = this->queue.capacity();
    const size_t window_size = capacity / 2;
    const size_t total_iterations = capacity * 3;

    for (std::size_t i{0}; i < window_size; ++i) {
        EXPECT_TRUE(this->queue.try_emplace(GeneratePayload<T>(i)));
    }

    for (size_t i{window_size}; i < total_iterations; ++i) {
        EXPECT_TRUE(this->queue.try_emplace(GeneratePayload<T>(i))) << "Emplace failed at index " << i;
        auto result = this->queue.try_pop();
        EXPECT_TRUE(result.has_value()) << "Pop failed at index " << i;
        EXPECT_EQ(result.value(), GeneratePayload<T>(i - window_size)) << "Data corrupted at index " << i;
    }

    for (size_t i{total_iterations - window_size}; i < total_iterations; ++i) {
        auto result = this->queue.try_pop();
        EXPECT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), GeneratePayload<T>(i));
    }
    EXPECT_FALSE(this->queue.try_pop().has_value();
}

// Register the basic test list
REGISTER_TYPED_TEST_SUITE_P(SPSC_basic_pattern,
    EmptyStateTest, SinglePushPopTest, FullBoundaryTest, WrapAroundChasing);


/// ============================================================================
/// 2. PATTERN: MOVE-ONLY TYPES
/// ============================================================================

template <typename QueueType>
class SPSC_move_pattern : public ::testing::Test {
protected:
    QueueType queue;
};

TYPED_TEST_SUITE_P(SPSC_move_pattern);

TYPED_TEST_P(SPSC_move_pattern, UniqPtrEmplaceTest) {
    using T = typename TypeParamTrait<TypeParam>::Type;
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
    EXPECT_TRUE(result.has_value());
    EXPECT_NE(result.value(), nullptr);
    EXPECT_EQ(*result.value(), test_val);
}

REGISTER_TYPED_TEST_SUITE_P(SPSC_move_pattern, UniqPtrEmplaceTest);


/// ============================================================================
/// 3. PATTERN: CONCURRENCY
/// ============================================================================

template <typename QueueType>
class SPSC_conc_pattern : public ::testing::Test {
protected:
    QueueType queue;
};

TYPED_TEST_SUITE_P(SPSC_conc_pattern);

TYPED_TEST_P(SPSC_conc_pattern, SeqIntegrityTest) {
    using T = typename TypeParamTrait<TypeParam>::Type;
    constexpr std::size_t ITEMS_TO_PROCESS{100'000}; // Dropped slightly for fast test iteration

    std::thread prod([&]() {
        for (std::size_t i{0}; i < ITEMS_TO_PROCESS; i++) {
            auto val = GeneratePayload<T>(i);
            while (!this->queue.try_emplace(std::move(val))) {
                std::this_thread::yield();
            }
        }
    });

    std::thread cons([&]() {
        for (std::size_t i{0}; i < ITEMS_TO_PROCESS; i++) {
            std::optional<T> result;
            while (!((result = this->queue.try_pop()))) {
                std::this_thread::yield();
            }
            ASSERT_EQ(result.value(), GeneratePayload<T>(i)) << "Failed at seq: " << i;
        }
    });

    prod.join();
    cons.join();
}

REGISTER_TYPED_TEST_SUITE_P(SPSC_conc_pattern, SeqIntegrityTest);