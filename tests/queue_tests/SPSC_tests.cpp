#include <gtest/gtest.h>
#include "../../Queues/SPSC.h"
#include "../../Queues/SPSC_A.h"
#include "../../common/common_headers.h"

template<typename ConcreteQueue, typename Element>
struct TestBundle {
    using FabricatedQueue = ConcreteQueue;
    using ElementType = Element;
};

// generates dynamic payload data across tests
template<typename T>
T GeneratePayload(size_t index) {
    static_assert(std::is_arithmetic_v<T> || std::is_same_v<T, std::string>);

    if constexpr (std::is_same_v<T, std::string>) {
        return "payload_" + std::to_string(index);
    } else {
        return static_cast<T>(index);
    }
}


/// BASIC LOGIC AND WRAPAROUND TESTS \\\

template<typename Bundle>
class SPSC_basic_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(SPSC_basic_pattern);

TYPED_TEST_P(SPSC_basic_pattern, EmptyStateTest) {
    auto result = this->queue.try_pop();
    EXPECT_FALSE(result.has_value());
}

TYPED_TEST_P(SPSC_basic_pattern, SinglePushPopTest) {
    using T = typename TypeParam::ElementType;

    auto val = GeneratePayload<T>(1);
    EXPECT_TRUE(this->queue.try_emplace(val));

    auto result = this->queue.try_pop();
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), val);
    EXPECT_FALSE(this->queue.try_pop().has_value());
}

TYPED_TEST_P(SPSC_basic_pattern, FullBoundaryTest) {
    using T = typename TypeParam::ElementType;

    for (std::size_t i{0}; i < this->queue.capacity(); i++) {
        EXPECT_TRUE(this->queue.try_emplace(T()));
    }
    EXPECT_FALSE(this->queue.try_emplace(T()));
}

TYPED_TEST_P(SPSC_basic_pattern, WrapAroundChasing) {
    using T = typename TypeParam::ElementType;

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
    EXPECT_FALSE(this->queue.try_pop().has_value());
}

REGISTER_TYPED_TEST_SUITE_P(SPSC_basic_pattern, EmptyStateTest, SinglePushPopTest, FullBoundaryTest, WrapAroundChasing);


/// MOVE ONLY TYPES TEST \\\

template<typename Bundle>
class SPSC_move_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(SPSC_move_pattern);

TYPED_TEST_P(SPSC_move_pattern, UniqPtrEmplaceTest) {
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
    EXPECT_TRUE(result.has_value());
    EXPECT_NE(result.value(), nullptr);
    EXPECT_EQ(*result.value(), test_val);
}

REGISTER_TYPED_TEST_SUITE_P(SPSC_move_pattern, UniqPtrEmplaceTest);


/// CONC TEST \\\

template<typename Bundle>
class SPSC_conc_pattern : public ::testing::Test {
protected:
    typename Bundle::FabricatedQueue queue;
};

TYPED_TEST_SUITE_P(SPSC_conc_pattern);

TYPED_TEST_P(SPSC_conc_pattern, SeqIntegrityTest) {
    using T = typename TypeParam::ElementType;
    constexpr std::size_t ITEMS_TO_PROCESS{100'000};

    std::thread prod([&]() {
        for (std::size_t i{0}; i < ITEMS_TO_PROCESS; i++) {
            auto val = GeneratePayload<T>(i);
            while (!this->queue.try_emplace(GeneratePayload<T>(i))) {
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


// SPSC test inst
using SPSC_Basic_Instances =
        ::testing::Types<TestBundle<tstl::lockfree::SPSC<int, 16>, int>, TestBundle<tstl::lockfree::SPSC<double, 16>, double>,
                         TestBundle<tstl::lockfree::SPSC<char, 16>, char>, TestBundle<tstl::lockfree::SPSC<std::string, 16>, std::string>>;

using SPSC_Move_Instances = ::testing::Types<TestBundle<tstl::lockfree::SPSC<std::unique_ptr<int>, 16>, int>,
                                             TestBundle<tstl::lockfree::SPSC<std::unique_ptr<double>, 16>, double>,
                                             TestBundle<tstl::lockfree::SPSC<std::unique_ptr<std::string>, 16>, std::string>>;

using SPSC_Conc_Instances =
        ::testing::Types<TestBundle<tstl::lockfree::SPSC<int, 1024>, int>, TestBundle<tstl::lockfree::SPSC<double, 1024>, double>,
                         TestBundle<tstl::lockfree::SPSC<char, 1024>, char>,
                         TestBundle<tstl::lockfree::SPSC<std::string, 1024>, std::string>>;

INSTANTIATE_TYPED_TEST_SUITE_P(SPSC_Original, SPSC_basic_pattern, SPSC_Basic_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(SPSC_Original, SPSC_move_pattern, SPSC_Move_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(SPSC_Original, SPSC_conc_pattern, SPSC_Conc_Instances);


// SPSC_A test inst
using SPSC_A_Basic_Instances =
        ::testing::Types<TestBundle<tstl::SPSC_A<int, 16>, int>, TestBundle<tstl::SPSC_A<double, 16>, double>,
                         TestBundle<tstl::SPSC_A<char, 16>, char>,
                         TestBundle<tstl::SPSC_A<std::string, 16>, std::string>>;

using SPSC_A_Move_Instances = ::testing::Types<TestBundle<tstl::SPSC_A<std::unique_ptr<int>, 16>, int>,
                                               TestBundle<tstl::SPSC_A<std::unique_ptr<double>, 16>, double>,
                                               TestBundle<tstl::SPSC_A<std::unique_ptr<std::string>, 16>, std::string>>;

using SPSC_A_Conc_Instances =
        ::testing::Types<TestBundle<tstl::SPSC_A<int, 1024>, int>, TestBundle<tstl::SPSC_A<double, 1024>, double>,
                         TestBundle<tstl::SPSC_A<char, 1024>, char>,
                         TestBundle<tstl::SPSC_A<std::string, 1024>, std::string>>;

INSTANTIATE_TYPED_TEST_SUITE_P(SPSC_Allocated, SPSC_basic_pattern, SPSC_A_Basic_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(SPSC_Allocated, SPSC_move_pattern, SPSC_A_Move_Instances);
INSTANTIATE_TYPED_TEST_SUITE_P(SPSC_Allocated, SPSC_conc_pattern, SPSC_A_Conc_Instances);
