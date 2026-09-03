# locking::Queue
`tstl::locking::Queue` is a bounded, mutex-protected Multi-Producer Multi-Consumer (MPMC) queue.
It supports both blocking and non-blocking producer and consumer operations via a single shared mutex
and two condition variables.

---
## Note
Use `locking::Queue` when simplicity matters more than peak throughput. It uses a single mutex and
is the right default for low-contention workloads with a small number of producers and consumers
. For high-throughput or high-contention scenarios, prefer `locking::MPMC`.
---

## Overview
`tstl::locking::Queue` is a general-purpose bounded queue designed for correctness and flexibility
over raw throughput. Any number of producers and consumers may use it concurrently. A single
`std::mutex` serialises all access; two `std::condition_variable`s allow threads to sleep rather
than spin when the queue is full or empty.

The data structure uses a ring buffer of raw `Slot`s backed by uninitialized `std::byte` storage,
so elements are only constructed when enqueued and destroyed immediately after being dequeued.
Index wraparound is handled with a power-of-2 bitmask for a branchless modulo.

---

## Requirements
- **`T` Requirements**: `T` must satisfy `std::is_nothrow_move_constructible_v<T>`.

- **`SIZE` Requirements**: `SIZE` must be a power of 2. Defaults to `1024`.

- **`Allocator`**: Reserved template parameter, currently unused (`void`).

---

## Threading contract
This class supports a full **Multi-Producer, Multi-Consumer** concurrency model:

- **Producer Threads**: Any number of threads may call `try_emplace()` or `emplace()` concurrently.

- **Consumer Threads**: Any number of threads may call `try_pop()` or `pop()` concurrently.

All access is serialised through a single internal mutex. There are no data races regardless of the
number of concurrent callers.

---

## API reference

### Constructor and destructor

```cpp
Queue() = default;
~Queue();
```

- `Queue()`: Initializes the queue in an empty state. Both heads are zero-initialized. No elements
  are constructed.

- `~Queue()`: Drains the queue by repeatedly calling `try_pop()` until empty, ensuring all remaining
  objects are properly destroyed. Destruction is **not thread-safe** — all threads must have stopped
  using the queue before its destructor runs.

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

- **Behavior**: Acquires the mutex (blocking until it can, since rejecting on contention rather than
  on fullness would cause spurious failures). Once the lock is held, checks whether the queue is full.
  If not, constructs the element in-place and notifies one waiting consumer via `m_cv_not_empty`.

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

- **Behavior**: Acquires the mutex and waits on `m_cv_not_full` until `write_head - read_head < SIZE`.
  Once space is available, constructs the element in-place, increments `write_head`, and notifies one
  waiting consumer.

- **When to use**: When items must not be dropped and the caller can afford to sleep until the
  consumer catches up.

---

### try_pop
```cpp
[[nodiscard]] std::optional<T> try_pop();
```

Attempts to dequeue an element without blocking on an empty queue.

- **Returns**: A `std::optional<T>` containing the dequeued element, or `std::nullopt` if the queue
  was empty.

- **Behavior**: Acquires the mutex, checks whether `read_head == write_head`. If empty, returns
  `std::nullopt`. Otherwise, move-constructs the element out of the slot, calls `std::destroy_at`
  on the slot, advances `read_head`, and notifies one waiting producer via `m_cv_not_full`.

---

### pop
```cpp
[[nodiscard]] T pop();
```

Dequeues an element, blocking until one is available.

- **Returns**: The dequeued element by value.

- **Behavior**: Acquires the mutex and waits on `m_cv_not_empty` until `write_head != read_head`.
  Once an item is present, move-constructs the result, destroys the slot's object, increments
  `read_head`, and notifies one waiting producer.

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

- **Returns**: An approximate element count at the moment the lock was acquired. The value may be
  stale by the time it is returned to the caller.

- **Note**: Acquires the mutex internally, so this is not a free operation. Do not call in hot paths.

---

## Synchronisation design

A single `std::mutex` (`m_mutex`) serialises all reads and writes to both `m_write_head` and
`m_read_head`. Neither head needs to be atomic because they are only ever accessed under the lock.

Two condition variables handle blocking:

- `m_cv_not_empty`: producers signal this after every successful write; consumers wait on it in `pop()`.
- `m_cv_not_full`: consumers signal this after every successful read; producers wait on it in `emplace()`.

`try_emplace` and `try_pop` still acquire the mutex so that head reads and writes remain consistent,
but they do not wait on a condition variable if the queue is full or empty — they return immediately.

**Notification after unlock**: Both `try_emplace` and `emplace` call `notify_one` after releasing the
lock to avoid waking a consumer that would immediately re-block on the mutex held by the producer.

---

## Object lifetime

- **Storage**: Each `Slot` contains a raw `alignas(T) std::byte[sizeof(T)]` array. No elements are
  default-constructed at queue initialization.

- **Creation**: `std::construct_at` constructs the element directly into the slot's raw storage after
  the lock is acquired and the slot has been claimed.

- **Destruction**: `std::destroy_at` is called immediately after the element has been moved out of
  the slot in `try_pop` and `pop`.

- **Cleanup**: The destructor drains all remaining items via `try_pop()`, guaranteeing every live
  object's destructor runs before the queue's storage is released.

---

## Capacity and wraparound

The queue uses a pair of monotonically incrementing indices (`m_write_head`, `m_read_head`) that are
never reset. Fullness is detected by comparing their difference against `SIZE`:

```cpp
if (current_write - m_read_head >= SIZE) { /* full */ }
```

The slot index is derived by masking with `SIZE - 1`:

```cpp
const std::size_t slot = current_index & (SIZE - 1);
```

Because both indices are plain `std::size_t` values that wrap on overflow in the same way, the
difference arithmetic remains correct across the natural wraparound of `std::size_t`.

---

## Exception guarantees

- **`try_emplace` / `emplace`**: Provide a **Strong Exception Guarantee**. `std::construct_at` is
  called before `m_write_head` is incremented. If the constructor throws, the head is not advanced
  and the queue state is fully preserved. The mutex is released via `std::unique_lock`'s destructor,
  so no lock is leaked.

- **`try_pop` / `pop`**: Provide a **No-Throw Guarantee** for the move step, enforced by the
  `std::is_nothrow_move_constructible_v<T>` constraint.

---

## Performance characteristics

Check [benchmarks](../../benchmarks)

---

## Limitations

- **Single mutex**: All producers and consumers contend on the same `m_mutex`. Under high concurrency
  this becomes a bottleneck. Consider `locking::MPSC` or `locking::MPMC` if producers and consumers
  can be split onto separate locks.

- **Fixed Size**: Capacity is fixed at compile time and cannot grow dynamically.

- **Move Semantics Required**: Objects must be nothrow move constructible.

---

## Testing and validation

Check [tests](../../tests)

---

## Example

Check [examples](../../examples)