#pragma once
#include "../allocator/slab_allocator.h"
#include "../common/common_headers.h"
#include "../common/macros.h"

namespace tstl {
    template<typename T, std::size_t SIZE = 1024, class Allocator = SlabAlloc<T, SIZE>>
    requires (std::is_nothrow_move_constructible_v<T>)
    class SPSC {
        // for fast bitwise masking
        static_assert((SIZE & (SIZE - 1)) == 0, "SPSC SIZE must be a power of 2");

    public:
        SPSC() : m_allocator(Allocator::create()) {
            for (auto &ptr: m_data) {
                ptr.store(nullptr, std::memory_order_relaxed);
            }
        }

        ~SPSC() {
            while (try_pop().has_value()) {}
        }

        // dont want to copy or move the queue (may add this facility later)
        SPSC(const SPSC &) = delete;
        SPSC &operator=(const SPSC &) = delete;

        SPSC(SPSC &&) = delete;
        SPSC &operator=(SPSC &&) = delete;

        // producer interface
        template<typename... Args>
        [[nodiscard]] bool try_emplace(Args &&...args) {
            const size_t current_write = write_head.load(std::memory_order_relaxed);

            if (TSTL_UNLIKELY(current_write - cached_read_head >= SIZE)) {
                cached_read_head = read_head.load(std::memory_order_acquire);
                if (current_write - cached_read_head >= SIZE) {
                    return false; // queue is full
                }
            }

            // not using the internal allocate_shared here because it would again cause the compiler to emit a LOCK
            // instruction
            T *item = m_allocator->std_allocator().allocate(1);
            if (!item) return false;

            std::construct_at(item, std::forward<Args>(args)...);
            m_data[current_write & (SIZE - 1)].store(item, std::memory_order_release);

            write_head.store(current_write + 1, std::memory_order_release);

            return true;
        }

        // consumer interface
        [[nodiscard]] std::optional<T> try_pop() {
            const size_t current_read = read_head.load(std::memory_order_relaxed);


            if (TSTL_UNLIKELY(current_read == cached_write_head)) {
                cached_write_head = write_head.load(std::memory_order_acquire);
                if (current_read == cached_write_head) {
                    return std::nullopt; //queue is empty
                }
            }

            const std::size_t slot = current_read & (SIZE - 1);

            T *item = m_data[slot].load(std::memory_order_acquire);
            T result = std::move(*item);

            std::destroy_at(item);
            m_allocator->std_allocator().deallocate(item, 1);

            read_head.store(current_read + 1, std::memory_order_release);

            return result;
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept { return SIZE; }
    private:
        std::array<std::atomic<T *>, SIZE> m_data;
        std::shared_ptr<Allocator> m_allocator{};

        // producer cache line
        alignas(detail::CACHE_LINE_SIZE) std::atomic<size_t> write_head{};
        size_t cached_read_head{0};

        // consumer cache line
        alignas(detail::CACHE_LINE_SIZE) std::atomic<size_t> read_head{};
        size_t cached_write_head{0};
    };
} // namespace tstl
