# locking::MPSC
`tstl::locking::MPSC` is a bounded, mutex-protected Multi-Producer Single-Consumer (MPSC) queue.
Producers are serialised by a dedicated write-side mutex; the single consumer operates independently
on a separate read-side mutex, allowing producers and the consumer to make progress concurrently.

---

## Overview
`tstl::locking::MPSC` is a bounded queue optimised for the common pattern where many threads produce
work and one thread drains it. By splitting the lock into a producer-side `m_write_mutex` and a
consumer-side `m_read_mutex`, producers and the consumer can proceed without blocking each other in
the common case.

Each side also maintains a **cached copy** of the other side's head (`m_read_head_cache` on the
producer side, `m_write_head_cache` on the consumer side). These caches avoid crossing to the other
side's cache line on every operation; an atomic reload from the true head only happens when the
cached value suggests the queue is full or empty.

The heads themselves are `std::atomic<std::size_t>` so that the cache-reload path can observe the
other side's latest value without holding the other side's lock.

---

## Requirements
- **`T` Requirements**: `T` must satisfy `std::is_nothrow_move_constructible_v<T>`.

- **`SIZE` Requirements**: `SIZE` must be a power of 2. Defaults to `1024`.

- **`Allocator`**: Reserved template parameter, currently unused (`void`).

---

## Threading contract
This class enforces a strict **Multi-Producer, Single-Consumer** concurrency model:

- **Producer Threads**: Any number of threads may call `try_emplace()` or `emplace()` concurrently.
  They are serialised by `m_write_mutex`.

- **Consumer Thread**: Only one specific thread may call `try_pop()` or `pop()` at any given time.
  The consumer has exclusive access to `m_read_head` and needs no contention on the read side.

**Violation**: Concurrent calls to `try_pop()` or `pop()` by multiple threads will result in data
races and undefined behavior.

---

## API reference

### Constructor and destructor

```cpp
MPSC() = default;
~MPSC();
```

- `MPSC()`: Initializes the queue in an empty state. Both heads and both caches are zero-initialized.

- `~MPSC()`: Drains the queue by repeatedly calling `try_pop()` until empty. Destruction is
  **not thread-safe** — all producer and consumer threads must have stopped using the queue before
  its destructor runs.

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

- **Behavior**: Acquires `m_write_mutex`. Checks fullness using the cached read head; if the cache
  suggests full, reloads `m_read_head` with `memory_order_acquire` for a definitive check. If
  genuinely full, returns `false`. Otherwise constructs the element in-place and releases
  `m_write_head` with `memory_order_release`. After dropping the write lock, briefly acquires
  `m_read_mutex` (then releases it immediately) before calling `notify_one` on `m_cv_not_empty` —
  this ensures the notification is mutually exclusive with the consumer's condition-variable predicate
  check, preventing a missed wakeup.

- **When to use**: When the caller must not sleep but can tolerate dropped items on a full queue.

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
  in-place and releases `m_write_head`. Notifies the consumer via `m_cv_not_empty` using the same
  lock-acquire-then-release pattern as `try_emplace` to prevent missed wakeups.

- **When to use**: When items must not be dropped and the producer can afford to sleep.

---

### try_pop
```cpp
[[nodiscard]] std::optional<T> try_pop();
```

Attempts to dequeue an element without blocking.

- **Returns**: A `std::optional<T>` containing the dequeued element, or `std::nullopt` if the queue
  was empty.

- **Behavior**: Operates entirely without acquiring any read-side mutex, since there is only one
  consumer. Checks emptiness using the cached write head; if the cache suggests empty, reloads
  `m_write_head` with `memory_order_acquire` for a definitive check. If empty, returns `std::nullopt`.
  Otherwise, move-constructs the element out of the slot, destroys the slot's object, and releases
  `m_read_head` with `memory_order_release`. Then briefly acquires `m_write_mutex` (without doing any
  work under it) before calling `notify_one` on `m_cv_not_full` to wake any blocked producer,
  using the same missed-wakeup prevention pattern.

- **Note**: Because there is only one consumer, `m_read_head` is advanced with a plain atomic store
  rather than a `fetch_add`.

---

### pop
```cpp
[[nodiscard]] T pop();
```

Dequeues an element, blocking until one is available.

- **Returns**: The dequeued element by value.

- **Behavior**: Acquires `m_read_mutex` and waits on `m_cv_not_empty`, reloading `m_write_head`
  (acquire) inside the predicate. Once an item is confirmed present, releases the lock and delegates
  to `try_pop()` for the actual extraction.

- **When to use**: When the consumer must process every item and can afford to sleep while the queue
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
  is returned.

---

## Synchronisation design

The key design insight is **lock splitting**: producers share `m_write_mutex` with each other but
not with the consumer; the consumer uses `m_read_mutex` only to sleep on `m_cv_not_empty`. The
heads are atomic so each side can observe the other's progress without crossing into the other's
critical section for every operation.

**Cached heads** reduce cross-cache-line traffic: `m_read_head_cache` (on the producer's cache line)
is refreshed from `m_read_head` only when the producer thinks the queue might be full. Symmetrically,
`m_write_head_cache` (on the consumer's cache line) is refreshed from `m_write_head` only when the
consumer thinks the queue might be empty.

**Missed-wakeup prevention**: A condition variable's `notify_one` can race with the waiting thread's
predicate evaluation. To close this window, both `try_emplace`/`emplace` (notifying the consumer)
and `try_pop` (notifying producers) briefly acquire the opposite side's mutex before calling
`notify_one`. This makes the notification and the predicate check mutually exclusive.

```
// Producer notifying consumer — acquire read lock first, then notify
{ std::lock_guard rlock(m_read_mutex); }
m_cv_not_empty.notify_one();

// Consumer notifying producers — acquire write lock first, then notify
{ std::lock_guard wlock(m_write_mutex); }
m_cv_not_full.notify_one();
```

---

## Object lifetime

- **Storage**: Each `Slot` holds a raw `alignas(T) std::byte[sizeof(T)]` array. No elements are
  default-constructed at initialization.

- **Creation**: `std::construct_at` constructs the element directly into the slot's raw storage
  while holding `m_write_mutex`.

- **Destruction**: `std::destroy_at` is called in `try_pop` immediately after the element has been
  moved out of the slot.

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
  the queue state is fully preserved. The mutex is released safely via RAII.

- **`try_pop` / `pop`**: Provide a **No-Throw Guarantee** for the move step, enforced by the
  `std::is_nothrow_move_constructible_v<T>` constraint.

---

## Performance characteristics

Check [benchmarks](../../benchmarks)

---

## Limitations

- **Fixed Size**: Capacity is fixed at compile time and cannot grow dynamically.

- **MPSC Only**: Multiple concurrent consumers will race on `m_read_head` and cause undefined
  behavior. Use `locking::MPMC` if multiple consumers are required.

- **Move Semantics Required**: Objects must be nothrow move constructible.

- **Mutex overhead**: While the split-lock design reduces contention compared to a single global
  mutex, each enqueue still requires acquiring `m_write_mutex`. For maximum throughput with no
  blocking requirement, consider `lockfree::MPSC` instead.

---

## Testing and validation

Check [tests](../../tests)

---

## Example

Check [examples](../../examples)