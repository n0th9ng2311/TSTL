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
            }

            // notify under m_read_mutex so consumer's condvar predicate check and his notification are
            // mutually exclusive
            { std::lock_guard rlock(m_read_mutex); }
            m_cv_not_empty.notify_one();
            return true;
        }

        template<typename... Args>
        void emplace(Args &&...args) {
            {
                std::unique_lock wlock(m_write_mutex);
                m_cv_not_full.wait(wlock, [this] {
                    m_read_head_cache = m_read_head.load(std::memory_order_relaxed);
                    return m_write_head.load(std::memory_order_relaxed) - m_read_head_cache < SIZE;
                });

                const std::size_t cur = m_write_head.load(std::memory_order_acquire);
                T *item = reinterpret_cast<T *>(&m_data[cur & (SIZE - 1)].storage);
                std::construct_at(item, std::forward<Args>(args)...);
                m_write_head.store(cur + 1, std::memory_order_release);
            }
            { std::lock_guard rlock(m_read_mutex); }
            m_cv_not_empty.notify_one();
        }

        // non-blocking pop, returns nullopt if empty
        // MPSC has single consumer so no contention on the read side
        [[nodiscard]] std::optional<T> try_pop() {
            std::optional<T> result;
            {
                std::lock_guard rlock(m_read_mutex);

                const std::size_t cur = m_read_head.load(std::memory_order_relaxed);
                if (TSTL_UNLIKELY(cur == m_write_head_cache)) {
                    m_write_head_cache = m_write_head.load(std::memory_order_acquire);
                    if (cur == m_write_head_cache) {
                        return std::nullopt; // empty
                    }
                }

                T *item = reinterpret_cast<T *>(&m_data[cur & (SIZE - 1)].storage);
                result = std::move(*item);
                std::destroy_at(item);
                m_read_head.store(cur + 1, std::memory_order_release);
            }

            { std::lock_guard wlock(m_write_mutex); }
            m_cv_not_full.notify_one();
            return result;
        }


        // blocking pop, sleeps under m_read_mutex
        [[nodiscard]] T pop() {
            std::optional<T> result;

            {
                std::unique_lock rlock(m_read_mutex);
                m_cv_not_empty.wait(rlock, [this] {
                    m_write_head_cache = m_write_head.load(std::memory_order_acquire);
                    return m_read_head.load(std::memory_order_relaxed) != m_write_head_cache;
                });

                const std::size_t cur = m_read_head.load(std::memory_order_relaxed);
                T *item = reinterpret_cast<T *>(&m_data[cur & (SIZE - 1)].storage);
                result = std::move(*item);
                std::destroy_at(item);
                m_read_head.store(cur + 1, std::memory_order_release);
            }

            { std::lock_guard wlock(m_write_mutex); }
            m_cv_not_full.notify_one();
            return std::move(*result);
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
