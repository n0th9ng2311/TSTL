#pragma once
#include "../common/common_headers.h"
#include "../common/macros.h"

namespace tstl::locking {

    template<typename T, std::size_t SIZE = 1024, class Allocator = void>
        requires(std::is_nothrow_move_constructible_v<T>)
    class MPSC {
        static_assert((SIZE & (SIZE - 1)) == 0, "MPSC SIZE must be a power of 2");

        struct Slot {
            alignas(T) std::byte storage[sizeof(T)];
        };

    public:
        MPSC() = default;
        ~MPSC() {
            while (try_pop().has_value()) {
            }
        }

        MPSC(const MPSC &) = delete;
        MPSC &operator=(const MPSC &) = delete;
        MPSC(MPSC &&) = delete;
        MPSC &operator=(MPSC &&) = delete;

        template<typename... Args>
        [[nodiscard]] bool try_emplace(Args &&...args) {
            {
                std::lock_guard lock(m_write_mutex);

                if (TSTL_UNLIKELY(m_write_head - m_read_head_cache >= SIZE)) {
                    // Refresh cached view of the read head
                    m_read_head_cache = m_read_head.load(std::memory_order_acquire);
                    if (m_write_head - m_read_head_cache >= SIZE) {
                        return false; // still full
                    }
                }

                const std::size_t slot = m_write_head & (SIZE - 1);
                T *item = reinterpret_cast<T *>(&m_data[slot].storage);
                std::construct_at(item, std::forward<Args>(args)...);
                m_write_head.store(m_write_head.load(std::memory_order_relaxed) + 1, std::memory_order_release);
            }
            m_cv_not_empty.notify_one();
            return true;
        }

        template<typename... Args>
        void emplace(Args &&...args) {
            // Spin-wait if full — producers don't sleep on a condvar here
            // because waking them correctly without data races requires the
            // consumer to hold m_write_mutex, which would kill parallelism.
            while (!try_emplace(std::forward<Args>(args)...)) {
                std::this_thread::yield();
            }
            m_cv_not_empty.notify_one();
        }

        [[nodiscard]] std::optional<T> try_pop() {
            const std::size_t current_read = m_read_head.load(std::memory_order_relaxed);

            if (TSTL_UNLIKELY(current_read == m_write_head_cache)) {
                m_write_head_cache = m_write_head.load(std::memory_order_acquire);
                if (current_read == m_write_head_cache) {
                    return std::nullopt; // empty
                }
            }

            const std::size_t slot = current_read & (SIZE - 1);
            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            T result = std::move(*item);
            std::destroy_at(item);
            // Release so producers see the freed slot
            m_read_head.store(current_read + 1, std::memory_order_release);
            return result;
        }

        [[nodiscard]] T pop() {
            // Fast path
            if (auto val = try_pop()) {
                return std::move(*val);
            }
            // Slow path
            std::unique_lock lock(m_read_mutex);
            m_cv_not_empty.wait(lock, [this] {
                return m_read_head.load(std::memory_order_relaxed) != m_write_head.load(std::memory_order_acquire);
            });
            lock.unlock();
            return std::move(*try_pop());
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept { return SIZE; }

        //just an approx may not be exact at or after the moment it is called
        [[nodiscard]] std::size_t size_approx() const noexcept {
            return m_write_head.load(std::memory_order_acquire) - m_read_head.load(std::memory_order_acquire);
        }

    private:
        alignas(detail::CACHE_LINE_SIZE) std::array<Slot, SIZE> m_data;

        // Producer's line
        alignas(detail::CACHE_LINE_SIZE) std::mutex m_write_mutex;
        std::atomic<std::size_t> m_write_head{0};
        std::size_t m_read_head_cache{0};

        // Consumer's line
        alignas(detail::CACHE_LINE_SIZE) std::mutex m_read_mutex;
        std::condition_variable m_cv_not_empty;
        std::atomic<std::size_t> m_read_head{0};
        std::size_t m_write_head_cache{0};
    };

} // namespace tstl::locking
