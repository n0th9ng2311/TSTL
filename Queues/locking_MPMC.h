#pragma once
#include "../common/common_headers.h"
#include "../common/macros.h"

namespace tstl::locking {

    template<typename T, std::size_t SIZE = 1024, class Allocator = void>
        requires(std::is_nothrow_move_constructible_v<T>)
    class MPMC {
        static_assert((SIZE & (SIZE - 1)) == 0, "MPMC SIZE must be a power of 2");

        struct Slot {
            alignas(T) std::byte storage[sizeof(T)];
        };

    public:
        MPMC() = default;

        // not thread safe, dont destroy queue while in use
        ~MPMC() {
            while (try_pop().has_value()) {
            }
        }

        MPMC(const MPMC &) = delete;
        MPMC &operator=(const MPMC &) = delete;
        MPMC(MPMC &&) = delete;
        MPMC &operator=(MPMC &&) = delete;

        template<typename... Args>
        [[nodiscard]] bool try_emplace(Args &&...args) {
            {
                std::lock_guard lock(m_write_mutex);

                const std::size_t current_write = m_write_head.load(std::memory_order_relaxed);

                if (TSTL_UNLIKELY(current_write - m_read_head_cache >= SIZE)) {
                    m_read_head_cache = m_read_head.load(std::memory_order_acquire);
                    if (current_write - m_read_head_cache >= SIZE) {
                        return false; // full
                    }
                }

                const std::size_t slot = current_write & (SIZE - 1);
                T *item = reinterpret_cast<T *>(&m_data[slot].storage);
                std::construct_at(item, std::forward<Args>(args)...);
                m_write_head.store(current_write + 1, std::memory_order_release);
            } // release the mutex before notifying
            m_cv_not_empty.notify_one();
            return true;
        }

        template<typename... Args>
        void emplace(Args &&...args) {
            std::unique_lock lock(m_write_mutex);
            m_cv_not_full.wait(lock, [this] {
                m_read_head_cache = m_read_head.load(std::memory_order_acquire);
                return m_write_head.load(std::memory_order_relaxed) - m_read_head_cache < SIZE;
            });

            const std::size_t current_write = m_write_head.load(std::memory_order_relaxed);
            const std::size_t slot = current_write & (SIZE - 1);
            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            std::construct_at(item, std::forward<Args>(args)...);
            m_write_head.store(current_write + 1, std::memory_order_release);
            lock.unlock();

            m_cv_not_empty.notify_one();
        }

        [[nodiscard]] std::optional<T> try_pop() {
            std::optional<T> result;
            {
                std::lock_guard lock(m_read_mutex);

                const std::size_t current_read = m_read_head.load(std::memory_order_relaxed);

                if (TSTL_UNLIKELY(current_read == m_write_head_cache)) {
                    m_write_head_cache = m_write_head.load(std::memory_order_acquire);
                    if (current_read == m_write_head_cache) {
                        return std::nullopt; // empty
                    }
                }

                const std::size_t slot = current_read & (SIZE - 1);
                T *item = reinterpret_cast<T *>(&m_data[slot].storage);
                result = std::move(*item);
                std::destroy_at(item);
                m_read_head.store(current_read + 1, std::memory_order_release);
            } // release the mutex before notifying
            m_cv_not_full.notify_one();
            return result;
        }

        [[nodiscard]] T pop() {
            std::unique_lock lock(m_read_mutex);
            m_cv_not_empty.wait(lock, [this] {
                m_write_head_cache = m_write_head.load(std::memory_order_acquire);
                return m_read_head.load(std::memory_order_relaxed) != m_write_head_cache;
            });

            const std::size_t current_read = m_read_head.load(std::memory_order_relaxed);
            const std::size_t slot = current_read & (SIZE - 1);
            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            T result = std::move(*item);
            std::destroy_at(item);
            m_read_head.store(current_read + 1, std::memory_order_release);
            lock.unlock();

            // Wake exactly one sleeping producer
            m_cv_not_full.notify_one();
            return result;
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept { return SIZE; }

        // just an approx may not be exact at or after the moment it is called
        [[nodiscard]] std::size_t size_approx() const noexcept {
            return m_write_head.load(std::memory_order_acquire) - m_read_head.load(std::memory_order_acquire);
        }

    private:
        alignas(detail::CACHE_LINE_SIZE) std::array<Slot, SIZE> m_data;

        // producer's line
        alignas(detail::CACHE_LINE_SIZE) std::mutex m_write_mutex;
        std::condition_variable m_cv_not_full;
        std::atomic<std::size_t> m_write_head{0};
        std::size_t m_read_head_cache{0};

        // consumer's line
        alignas(detail::CACHE_LINE_SIZE) std::mutex m_read_mutex;
        std::condition_variable m_cv_not_empty;
        std::atomic<std::size_t> m_read_head{0};
        std::size_t m_write_head_cache{0};
    };

} // namespace tstl::locking
