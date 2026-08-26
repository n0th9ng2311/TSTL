#pragma once
#include "../common/common_headers.h"
#include "../common/macros.h"


namespace tstl::lockfree {
    template<typename T, std::size_t SIZE>
        requires(std::is_nothrow_move_constructible_v<T>)
    class MPSC {
        static_assert((SIZE & (SIZE - 1)) == 0, "MPSC SIZE must be a power of 2");

        // slot also has to be aligned to cacheline size because for smaller types, multiple slots could fit inside one
        // cache line this will cause bouncing because of multiple producers trying to push values to adjacent
        // slots in same cacheline
        // Also initially I was using States like EMPTY and TAKEN to mark slots but, I found this better approach by
        // Dmitry Vyukov where we mark each index with a seq corresponding to it. (more about this in the blog)
        struct alignas(detail::CACHE_LINE_SIZE) Slot {
            std::atomic<std::size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)]{};
        };

    public:
        MPSC() {
            for (std::size_t i{0}; i < SIZE; ++i) {
                m_data[i].sequence.store(i, std::memory_order_relaxed);
            }
        }

        ~MPSC() {
            // this method will also reset the state back to 0 ATOMICALLY
            // also calling destructor while we still have producers running is UB
            while (try_pop().has_value()) {
            };
        }

        MPSC(const MPSC &) = delete;
        MPSC &operator=(const MPSC &) = delete;

        MPSC(MPSC &&) = delete;
        MPSC &operator=(MPSC &&) = delete;


        // Uses `fetch_add`. It NEVER abandons a ticket.
        // BUT:::: If the queue is full, the calling thread WILL spin-wait
        // infinitely until the consumer pops data. Use this when the consumer
        // is guaranteed to outpace the producers, or when tickets CANNOT be dropped
        template<typename... Args>
        void emplace(Args &&...args) {
            const std::size_t t = write_head.fetch_add(1, std::memory_order_relaxed);
            const std::size_t slot = t & (SIZE - 1);

            while (m_data[slot].sequence.load(std::memory_order_acquire) != t) {
                detail::spin_hint();
            }

            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            std::construct_at(item, std::forward<Args>(args)...);

            m_data[slot].sequence.store(t + 1, std::memory_order_release);
        }

        // Checks bounds BEFORE claiming a ticket. Returns false if full.
        // BUT:::: CAS loop. Under extreme load,this causes the write_head cache line to bounce,
        // slightly lowering max throughput. Use this for non-critical stuff or UI events
        // where it's better to drop data than block the calling thread.
        template<typename... Args>
        [[nodiscard]] bool try_emplace(Args &&...args) {
            std::size_t current_write = write_head.load(std::memory_order_relaxed);

            while (true) {
                const std::size_t slot = current_write & (SIZE - 1);
                const std::size_t seq = m_data[slot].sequence.load(std::memory_order_acquire);

                const std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(current_write);

                if (dif == 0) {
                    if (write_head.compare_exchange_weak(current_write, current_write + 1, std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
                        break; // success
                    }
                } else if (dif < 0) {
                    // The consumer hasn't recycled this slot.
                    // The queue is full.
                    return false;
                } else {
                    // Another producer already took this slot .
                    // current_write is stale, load a fresh one
                    current_write = write_head.load(std::memory_order_relaxed);
                }
            }

            const std::size_t slot = current_write & (SIZE - 1);

            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            std::construct_at(item, std::forward<Args>(args)...);

            m_data[slot].sequence.store(current_write + 1, std::memory_order_release);
            return true;
        }

        [[nodiscard]] std::optional<T> try_pop() {
            const std::size_t r = read_head.load(std::memory_order_relaxed);
            const std::size_t slot = r & (SIZE - 1);

            if (m_data[slot].sequence.load(std::memory_order_acquire) != r + 1) {
                return std::nullopt; // Queue empty, or producer is mid-write
            }

            T *item = reinterpret_cast<T *>(&m_data[slot].storage);
            T result = std::move(*item);
            std::destroy_at(item);

            // unlocking at r + SIZE
            m_data[slot].sequence.store(r + SIZE, std::memory_order_release);

            // not fetch and add because exclusive write priveleges
            read_head.store(r + 1, std::memory_order_relaxed);
            return result;
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept { return SIZE; }


    private:
        std::array<Slot, SIZE> m_data;

        alignas(detail::CACHE_LINE_SIZE) std::atomic<std::size_t> write_head{};
        alignas(detail::CACHE_LINE_SIZE) std::atomic<std::size_t> read_head{};
    };
} // namespace tstl::lockfree
