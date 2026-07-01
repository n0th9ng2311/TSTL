#pragma once
#include "../common/common_headers.h"
#include "../common/macros.h"


namespace tstl::lockfree {
    template<typename T, std::size_t SIZE = 1024, class Allocator = void>
        requires(std::is_nothrow_move_constructible_v<T>)
    class SPSC {
        static_assert((SIZE & (SIZE - 1)) == 0, "SPSC SIZE must be a power of 2");

        // inline storage slot that prevents default construction
        struct Slot {
            alignas(T) std::byte storage[sizeof(T)];
        };

    public:
        SPSC() = default;

        ~SPSC() {
            while (try_pop().has_value()) {
            }
        }

        SPSC(const SPSC &) = delete;
        SPSC &operator=(const SPSC &) = delete;

        SPSC(SPSC &&) = delete;
        SPSC &operator=(SPSC &&) = delete;

        template<typename... Args>
        [[nodiscard]] bool try_emplace(Args &&...args) {
            const std::size_t current_write = write_head.load(std::memory_order_relaxed);

            if (TSTL_UNLIKELY(current_write - cached_read_head >= SIZE)) {
                cached_read_head = read_head.load(std::memory_order_acquire);
                if (current_write - cached_read_head >= SIZE) {
                    return false;
                }
            }

            const std::size_t slot = current_write & (SIZE - 1);

            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            std::construct_at(item, std::forward<Args>(args)...);

            write_head.store(current_write + 1, std::memory_order_release);

            return true;
        }

        [[nodiscard]] std::optional<T> try_pop() {
            const std::size_t current_read = read_head.load(std::memory_order_relaxed);

            if (TSTL_UNLIKELY(current_read == cached_write_head)) {
                cached_write_head = write_head.load(std::memory_order_acquire);
                if (current_read == cached_write_head) {
                    return std::nullopt;
                }
            }

            const std::size_t slot = current_read & (SIZE - 1);

            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            T result = std::move(*item);

            std::destroy_at(item);
            read_head.store(current_read + 1, std::memory_order_release);

            return result;
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept { return SIZE; }

    private:
        std::array<Slot, SIZE> m_data;

        alignas(detail::CACHE_LINE_SIZE) std::atomic<std::size_t> write_head{};
        std::size_t cached_read_head{0};

        alignas(detail::CACHE_LINE_SIZE) std::atomic<std::size_t> read_head{};
        std::size_t cached_write_head{0};
    };
} // namespace tstl::lockfree

