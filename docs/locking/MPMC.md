# locking::MPMC
`tstl::locking::MPMC` is a bounded, mutex-protected Multi-Producer Multi-Consumer (MPMC) queue.
Producers are serialised by a dedicated write-side mutex and consumers by a separate read-side mutex,
allowing the producer and consumer sides to make progress independently.

---

## Overview
`tstl::locking::MPMC` is a bounded queue for the general case where both sides of the queue have
multiple concurrent threads. Like `locking::MPSC`, it employs **split locking**: `m_write_mutex`
serialises all producers and `m_read_mutex` serialises all consumers. Because the two sides hold
different locks, a producer and a consumer can execute their critical sections simultaneously as long
as neither needs to cross the cache-line boundary to check the other side's progress.

Each side caches the opposing head (`m_read_head_cache` on the write side, `m_write_head_cache` on
the read side) so that a full atomic reload from the other side's cache line only occurs when the
cached value suggests the queue is full or empty. The heads are `std::atomic<std::size_t>` so these
cache reloads can cross the mutex boundary safely.

---

## Requirements
- **`T` Requirements**: `T` must satisfy `std::is_nothrow_move_constructible_v<T>`.

- **`SIZE` Requirements**: `SIZE` must be a power of 2. Defaults to `1024`.

- **`Allocator`**: Reserved template parameter, currently unused (`void`).

---

## Threading contract
This class supports a full **Multi-Producer, Multi-Consumer** concurrency model:

- **Producer Threads**: Any number of threads may call `try_emplace()` or `emplace()` concurrently.
  They are serialised by `m_write_mutex`.

- **Consumer Threads**: Any number of threads may call `try_pop()` or `pop()` concurrently.
  They are serialised by `m_read_mutex`.

All operations are safe to call concurrently from any number of threads on either side.

---

## API reference

### Constructor and destructor

```cpp
MPMC() = default;
~MPMC();
```

- `MPMC()`: Initializes the queue in an empty state. Both atomic heads and both caches are
  zero-initialized. No elements are constructed.

- `~MPMC()`: Drains the queue by repeatedly calling `try_pop()` until empty, ensuring all remaining
  objects are properly destroyed. Destruction is **not thread-safe** — all producer and consumer
  threads must have stopped using the queue before its destructor runs.

- **Copy/Move Semantics**: The queue is strictly non-copyable and non-movable.

---

### try_emplace
```cpp
template<typename... Args>
[[nodiscard]] bool try_emplace(Args &&...args);
```

Attempts to enqueue an element without blocking on a full queue.

- **Parameters**: `args...` — Arguments forwarded to `T`'s constructor.

- **Returns**: `true` if the element was successfully enqueued. `false` if the queue is full.

- **Behavior**: Acquires `m_write_mutex`. Checks fullness using the cached read head
  (`m_read_head_cache`); if the cache suggests full, reloads `m_read_head` with
  `memory_order_acquire` for a definitive check. If genuinely full, returns `false`. Otherwise,
  constructs the element in-place and stores the new `m_write_head` with `memory_order_release`.
  After releasing the write lock, briefly acquires `m_read_mutex` (then releases it immediately)
  before calling `notify_one` on `m_cv_not_empty` — this prevents missed wakeups by making the
  notification mutually exclusive with the consumer's condition-variable predicate check.

- **When to use**: When the caller must not block but can tolerate dropped items on a full queue.

---

### emplace
```cpp
template<typename... Args>
void emplace(Args &&...args);
```

Enqueues an element, blocking until space is available.

- **Parameters**: `args...` — Arguments forwarded to `T`'s constructor.

- **Returns**: `void`.

- **Behavior**: Acquires `m_write_mutex` and waits on `m_cv_not_full`, reloading `m_read_head`
  (acquire) inside the predicate on each wakeup. Once space is confirmed, constructs the element
  in-place and releases `m_write_head`. Uses the same lock-acquire-then-notify pattern as
  `try_emplace` to signal the consumer without risking a missed wakeup.

- **When to use**: When items must not be dropped and the producer can afford to sleep.

---

### try_pop
```cpp
[[nodiscard]] std::optional<T> try_pop();
```

Attempts to dequeue an element without blocking.

- **Returns**: A `std::optional<T>` containing the dequeued element, or `std::nullopt` if the queue
  was empty.

- **Behavior**: Acquires `m_read_mutex`. Checks emptiness using the cached write head
  (`m_write_head_cache`); if the cache suggests empty, reloads `m_write_head` with
  `memory_order_acquire`. If genuinely empty, returns `std::nullopt`. Otherwise, move-constructs
  the element out of the slot, calls `std::destroy_at`, and releases `m_read_head` with
  `memory_order_release`. After releasing the read lock, briefly acquires `m_write_mutex` before
  notifying `m_cv_not_full` to wake any blocked producer.

- **Note**: Unlike `locking::MPSC`, `m_read_head` here is advanced with an atomic store under
  `m_read_mutex`, as multiple consumers may be present and the head must not be modified without
  the lock.

---

### pop
```cpp
[[nodiscard]] T pop();
```

Dequeues an element, blocking until one is available.

- **Returns**: The dequeued element by value.

- **Behavior**: Acquires `m_read_mutex` and waits on `m_cv_not_empty`, reloading `m_write_head`
  (acquire) inside the predicate. Once an item is confirmed, move-constructs the result, destroys
  the slot's object, advances `m_read_head`, releases the lock, and notifies one waiting producer
  via `m_cv_not_full`.

- **When to use**: When consumers must process every item and can afford to sleep while the queue
  is empty.

---

### capacity
```cpp
[[nodiscard]] static constexpr std::size_t capacity() noexcept;
```

- **Returns**: The fixed maximum number of elements the queue can hold, equal to the `SIZE` template
  parameter.

---

### size_approx
```cpp
[[nodiscard]] std::size_t size_approx() const noexcept;
```

- **Returns**: An approximate element count computed as `m_write_head - m_read_head` using
  `memory_order_acquire` loads on both. No lock is held, so the value may be stale by the time it
  is returned to the caller.

---

## Synchronisation design

**Split locking** is the central design decision. Producers share `m_write_mutex`; consumers share
`m_read_mutex`. The two mutexes are independent, so a producer mid-enqueue does not block a consumer
mid-dequeue and vice versa.

**Cached heads** reduce cross-cache-line traffic. The producer side caches the consumer's
`m_read_head` in `m_read_head_cache` and only reloads the atomic when it believes the queue may be
full. The consumer side caches the producer's `m_write_head` in `m_write_head_cache` and reloads
only when it believes the queue may be empty. This pattern means the common (non-boundary) case
requires no atomic acquire across the opposite cache line.

**Missed-wakeup prevention**: A `notify_one` can arrive between a waiter checking its predicate and
calling `wait`, causing the wakeup to be lost. To prevent this, the notifier briefly acquires the
opposite side's mutex before calling `notify_one`, making the notification and the predicate
evaluation mutually exclusive:

```cpp
// Producer notifying consumers — hold read lock momentarily
{ std::lock_guard rlock(m_read_mutex); }
m_cv_not_empty.notify_one();

// Consumer notifying producers — hold write lock momentarily
{ std::lock_guard wlock(m_write_mutex); }
m_cv_not_full.notify_one();
```

---

## Differences from locking::MPSC

| Aspect | `locking::MPSC` | `locking::MPMC` |
|---|---|---|
| Consumer count | Single | Multiple |
| `try_pop` / `pop` lock | No lock needed (single consumer) | Acquires `m_read_mutex` |
| `m_read_head` advancement | Plain atomic store, no lock | Atomic store under `m_read_mutex` |
| `m_read_mutex` purpose | Only used for `pop` sleep | Serialises all consumer access |

---

## Object lifetime

- **Storage**: Each `Slot` holds a raw `alignas(T) std::byte[sizeof(T)]` array. No elements are
  default-constructed at initialization.

- **Creation**: `std::construct_at` constructs the element directly into the slot's raw storage
  while holding `m_write_mutex`.

- **Destruction**: `std::destroy_at` is called immediately after the element has been moved out of
  the slot in both `try_pop` and `pop`.

- **Cleanup**: The destructor drains all remaining items via `try_pop()`, ensuring every live
  object's destructor runs before the queue's storage is released.

---

## Capacity and wraparound

Fullness is detected by comparing the difference of the two monotonically incrementing heads against `SIZE`:

```cpp
if (current_write - m_read_head_cache >= SIZE) { /* full */ }
```

The slot index is derived with a power-of-2 bitmask:

```cpp
const std::size_t slot = current_index & (SIZE - 1);
```

---

## Exception guarantees

- **`try_emplace` / `emplace`**: Provide a **Strong Exception Guarantee**. `std::construct_at` is
  called before `m_write_head` is updated. If the constructor throws, the head is not advanced and
  the queue state is fully preserved. All locks are released safely via RAII.

- **`try_pop` / `pop`**: Provide a **No-Throw Guarantee** for the move step, enforced by the
  `std::is_nothrow_move_constructible_v<T>` constraint.

---

## Performance characteristics

Check [benchmarks](../../benchmarks)

---

## Limitations

- **Fixed Size**: Capacity is fixed at compile time and cannot grow dynamically.

- **Producer contention**: All producers share `m_write_mutex`. Under high producer concurrency this
  lock becomes a serialisation bottleneck. For lock-free multi-producer scenarios with a single
  consumer, consider `lockfree::MPSC`.

- **Consumer contention**: All consumers share `m_read_mutex`. Under high consumer concurrency this
  lock becomes a bottleneck on the read side.

- **Move Semantics Required**: Objects must be nothrow move constructible.

---

## Testing and validation

Check [tests](../../tests)

---

## Example

Check [examples](../../examples)