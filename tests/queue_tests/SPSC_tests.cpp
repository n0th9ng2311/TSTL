#include <gtest/gtest.h>
#include "../../Queues/SPSC.h"
#include "../../common/common_headers.h"

using TestingTypes = ::testing::Types<int, double, char, std::string>;

/// BASIC LOGIC TEST ///

template<typename T>
class SPSC_basic_test_class : public ::testing::Test {
protected:
    tstl::SPSC<T, 16> queue; // smaller queue for boundary testing
};

TYPED_TEST_SUITE(SPSC_basic_test_class, TestingTypes);

TYPED_TEST(SPSC_basic_test_class, EmptyStateTest) {
    auto result = this->queue.try_pop();
    EXPECT_FALSE(result.has_value());
}

TYPED_TEST(SPSC_basic_test_class, SinglePushPopTest) {
    TypeParam val = TypeParam();
    EXPECT_TRUE(this->queue.try_emplace(val));

    auto result = this->queue.try_pop();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), val);

    EXPECT_FALSE(this->queue.try_pop().has_value());
}

TYPED_TEST(SPSC_basic_test_class, FullBoundaryTest) {
    for (std::size_t i{0}; i < 16; i++) {
        EXPECT_TRUE(this->queue.try_emplace(TypeParam()));
    }

    EXPECT_FALSE(this->queue.try_emplace(TypeParam()));
}


/// TEST ON MOVE ONLY TYPES ///

using MoveTestingTypes = ::testing::Types<int, double, std::string>;

template<typename T>
class SPSC_move_test_class : public ::testing::Test {
protected:
    tstl::SPSC<std::unique_ptr<T>, 16> queue;
};

TYPED_TEST_SUITE(SPSC_move_test_class, MoveTestingTypes);

TYPED_TEST(SPSC_move_test_class, UniqPtrEmplaceTest) {
    TypeParam test_val;
    if constexpr (std::is_arithmetic_v<TypeParam>) {
        test_val = 40;
    } else if constexpr (std::is_same_v<TypeParam, std::string>) {
        test_val = "40";
    }

    auto ptr = std::make_unique<TypeParam>(test_val);

    EXPECT_TRUE(this->queue.try_emplace(std::move(ptr)));
    EXPECT_EQ(ptr, nullptr);

    auto result = this->queue.try_pop();
    EXPECT_TRUE(result.has_value());
    EXPECT_NE(result.value(), nullptr);

    EXPECT_EQ(*result.value(), test_val);
}


/// TEST OF CONCURRENCY ///

template<typename T>
T GeneratePayload(size_t index) {
    if constexpr (std::is_same_v<T, std::string>) {
        return "payload_" + std::to_string(index);
    } else {
        return static_cast<T>(index);
    }
}

template<typename T>
class SPSC_conc_test_class : public ::testing::Test {
protected:
    tstl::SPSC<T> queue;
};

TYPED_TEST_SUITE(SPSC_conc_test_class, TestingTypes);

TYPED_TEST(SPSC_conc_test_class, SeqIntegrityTest) {
    constexpr std::size_t ITEMS_TO_PROCESS{1'000'000};

    std::thread prod([&]() {
        for (std::size_t i{0}; i < ITEMS_TO_PROCESS; i++) {
            auto val = GeneratePayload<TypeParam>(i);

            while (!this->queue.try_emplace(std::move(val))) {
                std::this_thread::yield();
            }
        }
    });

    std::thread cons([&]() {
        for (std::size_t i{0}; i < ITEMS_TO_PROCESS; i++) {
            std::optional<TypeParam> result;

            while (!((result = this->queue.try_pop()))) {
                std::this_thread::yield();
            }

            auto expected_val = GeneratePayload<TypeParam>(i);

            ASSERT_EQ(result.value(), expected_val) << "Failed at seq: " << i;
        }
    });

    prod.join();
    cons.join();
}


/// WRAP AROUND TESTS ///

TYPED_TEST(SPSC_basic_test_class, WrapAroundChasing) {
    const size_t capacity = this->queue.capacity();
    const size_t window_size = capacity / 2;
    const size_t total_iterations = capacity * 3;

    for (std::size_t i{0}; i < window_size; ++i) {
        auto val = GeneratePayload<TypeParam>(i);
        EXPECT_TRUE(this->queue.try_emplace(std::move(val)));
    }

    for (size_t i{window_size}; i < total_iterations; ++i) {

        auto new_val = GeneratePayload<TypeParam>(i);
        EXPECT_TRUE(this->queue.try_emplace(std::move(new_val))) << "Emplace failed at index " << i;

        auto result = this->queue.try_pop();
        EXPECT_TRUE(result.has_value()) << "Pop failed at index " << i;

        auto expected_val = GeneratePayload<TypeParam>(i - window_size);
        EXPECT_EQ(result.value(), expected_val) << "Data corrupted during wrap-around at index " << i;
    }

    for (size_t i{total_iterations - window_size}; i < total_iterations; ++i) {
        auto result = this->queue.try_pop();
        EXPECT_TRUE(result.has_value());

        auto expected_val = GeneratePayload<TypeParam>(i);
        EXPECT_EQ(result.value(), expected_val);
    }

    EXPECT_FALSE(this->queue.try_pop().has_value()); // que should be empty
}
