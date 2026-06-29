#pragma once
#include "../common/common_headers.h"

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#endif

namespace detail {
    inline void spin_hint() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#elif defined(__arm__) || defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }

#if defined(__cpp_lib_hardware_interference_size)
    inline constexpr std::size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#elif defined(__aarch64__) || defined(_M_ARM64)
    inline constexpr std::size_t CACHE_LINE_SIZE = 128; // Apple Silicon / Graviton
#else
    inline constexpr std::size_t CACHE_LINE_SIZE = 64; // x86_64 / Default
#endif
} // namespace detail

using u64 = std::uint64_t;


namespace tstl {
    template<typename T, u64 SLAB_SIZE = 1024>
    class SlabAlloc : public std::enable_shared_from_this<SlabAlloc<T, SLAB_SIZE>> {
        static_assert(SLAB_SIZE > 0, "SLAB_SIZE must be at least 1");

        struct Slot {
            static constexpr std::size_t BUF = sizeof(T) >= sizeof(void *) ? sizeof(T) : sizeof(void *);

            alignas(std::max(alignof(T), alignof(void *))) std::byte m_data[BUF];

            void store_next(Slot *next) noexcept { std::memcpy(m_data, &next, sizeof(void *)); }

            [[nodiscard]] Slot *load_next() const noexcept {
                Slot *next{};
                std::memcpy(&next, m_data, sizeof(void *));
                return next;
            }
        };

    public:
        [[nodiscard]] static std::shared_ptr<SlabAlloc> create() { return std::shared_ptr<SlabAlloc>(new SlabAlloc()); }

        // Don't need to copy or move the slab
        SlabAlloc(const SlabAlloc &) = delete;
        SlabAlloc &operator=(const SlabAlloc &) = delete;
        SlabAlloc(SlabAlloc &&) = delete;
        SlabAlloc &operator=(SlabAlloc &&) = delete;

        ~SlabAlloc() { delete[] m_slots; }

        template<typename... Args>
        [[nodiscard]] std::shared_ptr<T> allocate_shared(Args &&...args) {
            Slot *slot = pop_slot();
            T *raw = nullptr;
            bool constructed = false; // Checks if the object was full constructed or not

            try {
                raw = std::construct_at(reinterpret_cast<T *>(slot->m_data), std::forward<Args>(args)...);
                constructed = true;

                auto self = this->shared_from_this(); // Can throw std::bad_weak_ptr

                // Can throw std::bad_alloc during internal control block allocation
                return std::shared_ptr<T>(raw, [self, slot](T *p) noexcept {
                    std::destroy_at(p);
                    self->push_slot(slot);
                });
            } catch (...) {
                if (constructed && raw) {
                    std::destroy_at(raw);
                }
                push_slot(slot);
                throw;
            }
        }

        [[nodiscard]] u64 capacity() const noexcept { return SLAB_SIZE; }
        [[nodiscard]] u64 available() const noexcept { return m_available.load(std::memory_order_relaxed); }


        template<typename U = T>
        class StdAllocator {
        public:
            using value_type = U;
            using size_type = std::size_t;
            using difference_type = std::ptrdiff_t;

            using propagate_on_container_copy_assignment = std::true_type;
            using propagate_on_container_move_assignment = std::true_type;
            using propagate_on_container_swap = std::true_type;

            explicit StdAllocator(std::shared_ptr<SlabAlloc> slab) noexcept : m_slab(std::move(slab)) {}

            template<typename V>
            explicit StdAllocator(const StdAllocator<V> &other) noexcept : m_slab(other.slab()) {}

            [[nodiscard]] U *allocate(std::size_t n) {
                if constexpr (std::is_same_v<U, T>) {
                    if (n == 1) {
                        Slot *slot = m_slab->pop_slot();
                        return reinterpret_cast<U *>(slot->m_data);
                    }
                }

                // Handles over-aligned types
                if constexpr (alignof(U) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
                    return static_cast<U *>(::operator new(n * sizeof(U), std::align_val_t{alignof(U)}));
                } else {
                    return static_cast<U *>(::operator new(n * sizeof(U)));
                }
            }

            void deallocate(U *p, std::size_t n) noexcept {
                if constexpr (std::is_same_v<U, T>) {
                    if (n == 1 && m_slab->owns(p)) {
                        static_assert(offsetof(Slot, m_data) == 0, "m_data must be the first Slot member");
                        m_slab->push_slot(reinterpret_cast<Slot *>(p));
                        return;
                    }
                }

                if constexpr (alignof(U) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
                    ::operator delete(p, n * sizeof(U), std::align_val_t{alignof(U)});
                } else {
                    ::operator delete(p, n * sizeof(U));
                }
            }

            template<typename W>
            struct rebind {
                using other = StdAllocator<W>;
            };

            template<typename V>
            friend bool operator==(const StdAllocator &a, const StdAllocator<V> &b) noexcept {
                return a.m_slab == b.slab();
            }
            template<typename V>
            friend bool operator!=(const StdAllocator &a, const StdAllocator<V> &b) noexcept {
                return !(a == b);
            }

            [[nodiscard]] std::shared_ptr<SlabAlloc> slab() const noexcept { return m_slab; }

        private:
            template<typename>
            friend class StdAllocator;
            std::shared_ptr<SlabAlloc> m_slab;
        };

        [[nodiscard]] StdAllocator<T> std_allocator() { return StdAllocator<T>(this->shared_from_this()); }

    private:
        SlabAlloc() : m_available(SLAB_SIZE) {
            m_slots = new Slot[SLAB_SIZE];
            m_free_list = &m_slots[0];
            for (u64 i{0}; i < SLAB_SIZE - 1; ++i)
                m_slots[i].store_next(&m_slots[i + 1]);
            m_slots[SLAB_SIZE - 1].store_next(nullptr);
        }

        struct SpinGuard {
            std::atomic_flag &flag;
            explicit SpinGuard(std::atomic_flag &f) noexcept : flag(f) {
                while (true) {
                    // Read-only (TTAS)
                    if (!flag.test(std::memory_order_relaxed)) {
                        if (!flag.test_and_set(std::memory_order_acquire)) {
                            break;
                        }
                    }
                    detail::spin_hint();
                }
            }
            ~SpinGuard() noexcept { flag.clear(std::memory_order_release); }
            SpinGuard(const SpinGuard &) = delete;
            SpinGuard &operator=(const SpinGuard &) = delete;
        };

        [[nodiscard]] Slot *pop_slot() {
            SpinGuard g(m_lock);
            Slot *slot = m_free_list;
            if (!slot)
                throw std::bad_alloc{};
            m_free_list = slot->load_next();
            // not using .fetch_sub because it makes the compiler emits a full LOCK instruction on x86 which can cause a
            // minor slowdown
            auto current = m_available.load(std::memory_order_relaxed);
            m_available.store(current - 1, std::memory_order_relaxed);
            return slot;
        }

        void push_slot(Slot *slot) noexcept {
            SpinGuard g(m_lock);
            slot->store_next(m_free_list);
            m_free_list = slot;
            // not using .fetch_add here for the same reason
            auto current = m_available.load(std::memory_order_relaxed);
            m_available.store(current + 1, std::memory_order_relaxed);
        }

        [[nodiscard]] bool owns(const void *p) const noexcept {
            const auto *byte_p = static_cast<const std::byte *>(p);
            const auto *slab_begin = reinterpret_cast<const std::byte *>(m_slots);
            const auto *slab_end = reinterpret_cast<const std::byte *>(m_slots + SLAB_SIZE);
            return byte_p >= slab_begin && byte_p < slab_end;
        }

        // Data layout isolated across separate cache lines(to deal with false sharing)
        alignas(detail::CACHE_LINE_SIZE) mutable std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
        Slot *m_slots{nullptr};
        Slot *m_free_list{nullptr};

        alignas(detail::CACHE_LINE_SIZE) std::atomic<u64> m_available{0};
    };
} // namespace tstl
