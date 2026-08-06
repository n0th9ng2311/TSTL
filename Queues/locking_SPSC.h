#pragma once
#include "../common/common_headers.h"
#include "../common/macros.h"

namespace tstl::locking {

    template<typename T, std::size_t SIZE = 1024, class Allocator = void>
        requires(std::is_nothrow_move_constructible_v<T>)
    class Queue {
        static_assert((SIZE & (SIZE - 1)) == 0, "Queue SIZE must be a power of 2");

        struct Slot {
            alignas(T) std::byte storage[sizeof(T)];
        };

    public:
        Queue() = default;
        ~Queue() {
            while (try_pop().has_value()) {
            }
        }

        Queue(const Queue &) = delete;
        Queue &operator=(const Queue &) = delete;
        Queue(Queue &&) = delete;
        Queue &operator=(Queue &&) = delete;

        template<typename... Args>
        [[nodiscard]] bool try_emplace(Args &&...args) {
            std::unique_lock lock(m_mutex, std::try_to_lock);
            if (TSTL_UNLIKELY(!lock.owns_lock())) {
                // oonly reject when the queue is 'full' not when another thread is inside
                lock.lock();
            }

            const std::size_t current_write = m_write_head;
            if (TSTL_UNLIKELY(current_write - m_read_head >= SIZE)) {
                return false; // queue full
            }

            const std::size_t slot = current_write & (SIZE - 1);
            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            std::construct_at(item, std::forward<Args>(args)...);
            m_write_head = current_write + 1;
            return true;
        }

        [[nodiscard]] std::optional<T> try_pop() {
            std::unique_lock lock(m_mutex, std::try_to_lock);
            if (TSTL_UNLIKELY(!lock.owns_lock())) {
                lock.lock();
            }

            const std::size_t current_read = m_read_head;
            if (TSTL_UNLIKELY(current_read == m_write_head)) {
                return std::nullopt; // queue empty
            }

            const std::size_t slot = current_read & (SIZE - 1);
            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            T result = std::move(*item);
            std::destroy_at(item);
            m_read_head = current_read + 1;
            return result;
        }

        // Blocking variants
        template<typename... Args>
        void emplace(Args &&...args) {
            while (!try_emplace(std::forward<Args>(args)...)) {
                m_cv_not_full.wait(std::unique_lock(m_mutex), [this] { return m_write_head - m_read_head < SIZE; });
            }
            m_cv_not_empty.notify_one();
        }

        [[nodiscard]] T pop() {
            std::unique_lock lock(m_mutex);
            m_cv_not_empty.wait(lock, [this] { return m_write_head != m_read_head; });

            const std::size_t slot = m_read_head & (SIZE - 1);
            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            T result = std::move(*item);
            std::destroy_at(item);
            ++m_read_head;

            lock.unlock();
            m_cv_not_full.notify_one();
            return result;
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept { return SIZE; }

        // Approximate
        [[nodiscard]] std::size_t size_approx() const noexcept {
            std::lock_guard lock(m_mutex);
            return m_write_head - m_read_head;
        }

    private:
        alignas(detail::CACHE_LINE_SIZE) std::array<Slot, SIZE> m_data;

        //producer's line
        std::size_t m_write_head{0};
        std::size_t m_read_head{0};

        //consumer's line
        alignas(detail::CACHE_LINE_SIZE) mutable std::mutex m_mutex;
        std::condition_variable m_cv_not_empty;
        std::condition_variable m_cv_not_full;
    };

} // namespace tstl::locking
