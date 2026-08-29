# lockfree::MPSC
`tstl::lockfree::MPSC` is a bounded, non-blocking Multi-Producer Single-Consumer (MPSC) queue. Its
operations return immediately when the queue is full or empty, and producers coordinate lock-free via
a sequence-number scheme inspired by Dmitry Vyukov's bounded MPMC queue.

---

## Overview
`tstl::lockfree::MPSC` is a high-performance, bounded, lock-free Multi-Producer Single-Consumer (MPSC) queue.
It is designed for ultra-low latency inter-thread communication where multiple producers feed a single consumer.

The data structure uses a ring buffer of cache-line-aligned `Slot`s, each carrying an atomic sequence number
instead of a state flag. This sequence-number design eliminates ABA problems and avoids cache-line bouncing
between adjacent slots written by competing producers. Index wraparound uses a power-of-2 bitmask so the
modulo operation is a single AND instruction.

---

## Requirements
- **`T` Requirements**: `T` must satisfy `std::is_nothrow_move_constructible_v<T>`. This guarantees that
  moving objects out of the queue during a pop operation will not throw.

- **`SIZE` Requirements**: `SIZE` must be a power of 2.

---

## Threading contract
This class enforces a strict **Multi-Producer, Single-Consumer** concurrency model:

- **Producer Threads**: Any number of threads may call `emplace()` or `try_emplace()` concurrently.

- **Consumer Thread**: Only one specific thread may call `try_pop()` at any given time.

**Violation**: Concurrent calls to `try_pop()` by multiple threads will result in data races, memory
corruption, and undefined behavior.

---

## API reference

### Constructor and destructor

```cpp
MPSC();
~MPSC();
```

- `MPSC()`: Initializes each slot's sequence number to its index (`sequence[i] = i`), marking all slots
  as empty and ready to accept their first write.

- `~MPSC()`: Drains the queue by repeatedly calling `try_pop()` until empty, ensuring all remaining
  objects are properly destroyed. Destruction is **not thread-safe** — all producer and consumer threads
  must have stopped using the queue before its destructor runs.

- **Copy/Move Semantics**: The queue is strictly non-copyable and non-movable.

---

### emplace
```cpp
template<typename... Args>
void emplace(Args &&...args);
```

Unconditionally enqueues an element, blocking if the queue is full.

- **Parameters**: `args...` — Arguments forwarded to `T`'s constructor.

- **Returns**: `void`.

- **Behavior**: Claims a ticket via `fetch_add` and spin-waits on the target slot's sequence number
  until the consumer has recycled it. Once the slot is ready, the element is constructed in-place and
  the sequence is released to signal the consumer.

- **When to use**: When the consumer is guaranteed to outpace the producers, or when tickets **must
  not** be dropped (e.g. critical event logging, command queues).

- **Warning**: If the queue remains full indefinitely, the calling thread will spin forever.
  Prefer `try_emplace()` in latency-sensitive or best-effort paths.

---

### try_emplace
```cpp
template<typename... Args>
[[nodiscard]] bool try_emplace(Args &&...args);
```

Attempts to enqueue an element, returning immediately if the queue is full.

- **Parameters**: `args...` — Arguments forwarded to `T`'s constructor.

- **Returns**: `true` if the element was successfully enqueued. `false` if the queue is full.

- **Behavior**: Uses a CAS loop to atomically claim a slot. The sequence number of the candidate slot
  is inspected before the CAS:
    - `dif == 0` — slot is free; attempt to claim it with `compare_exchange_weak`.
    - `dif < 0` — slot has not been recycled by the consumer; queue is full, return `false`.
    - `dif > 0` — another producer already claimed this slot; reload `write_head` and retry.

- **When to use**: For non-critical paths, UI events, or any scenario where dropping data is
  preferable to blocking the calling thread.

- **Trade-off**: Under extreme producer contention the CAS loop causes `write_head`'s cache line
  to bounce between cores, which slightly reduces peak throughput compared to `emplace()`.

---

### try_pop
```cpp
[[nodiscard]] std::optional<T> try_pop();
```

Attempts to extract the next element from the queue.

- **Returns**: A `std::optional<T>` containing the extracted element if the queue was not empty, or
  `std::nullopt` if the queue was empty (or a producer is mid-write on the next slot).

- **Behavior**: Reads the current `read_head`, checks whether the slot's sequence equals `r + 1`
  (the value a producer sets on a completed write). If so, the item is move-constructed out, the
  slot is recycled by storing `r + SIZE` into its sequence, and `read_head` is advanced. A plain
  `store` (not `fetch_add`) is used because the consumer has exclusive write privileges over
  `read_head`.

- **Performance**: Wait-free from the consumer's perspective. A single acquire-load on the slot
  sequence is sufficient; no CAS is required.

---

### capacity
```cpp
[[nodiscard]] static constexpr std::size_t capacity() noexcept;
```

- **Returns**: The fixed maximum number of elements the queue can hold, equal to the `SIZE` template
  parameter.

---

## Memory-ordering design

1. **Slot sequence as the synchronisation primitive**: Rather than a separate lock or state enum,
   each `Slot` carries an `std::atomic<std::size_t> sequence`. The relationship between the
   sequence value and the current indices encodes whether a slot is empty, being written, or
   ready to read — without any additional flags.

2. **Producer → Consumer (`release` / `acquire` on sequence)**:
    - After constructing the object, the producer calls `sequence.store(t + 1, memory_order_release)`.
    - The consumer checks `sequence.load(memory_order_acquire) == r + 1` before reading.
    - This acquire-release pair guarantees the fully constructed object is visible to the consumer
      before it observes the updated sequence.

3. **Consumer → Producer (`release` / `acquire` on sequence)**:
    - After moving the object out, the consumer calls `sequence.store(r + SIZE, memory_order_release)`
      to recycle the slot.
    - Producers observe this via `sequence.load(memory_order_acquire)` when computing `dif`, ensuring
      they never overwrite a slot that has not yet been consumed.

4. **`write_head` ordering**: `fetch_add` and the CAS in `try_emplace` use `memory_order_relaxed`
   because the sequence number on the slot itself is the authoritative synchronisation point; the
   head only needs to be atomically incremented, not to order any payload.

5. **False Sharing Prevention**: `write_head` and `read_head` are each `alignas(detail::CACHE_LINE_SIZE)`.
   Every `Slot` is also `alignas(detail::CACHE_LINE_SIZE)`, preventing two producers from contending
   on the same cache line when writing to adjacent slots.

---

## Object lifetime

The queue avoids default-constructing `SIZE` elements at initialization. Instead, each `Slot` holds a
raw `std::byte` array with `alignas(T)` alignment.

- **Creation**: `std::construct_at` inside `emplace` / `try_emplace` invokes the constructor directly
  in the raw memory after the slot has been claimed.

- **Destruction**: `std::destroy_at` is called inside `try_pop` immediately after the object has been
  moved out of the slot.

- **Cleanup**: The destructor drains any remaining items via `try_pop()`, guaranteeing every live
  object's destructor is called before the queue's storage is released.

---

## Capacity and wraparound

The queue acts as a ring buffer using a continuously incrementing absolute index masked to the buffer
bounds. Because `SIZE` is validated by `static_assert` to be a power of 2, the mapping from absolute
index to slot is a single bitwise AND:

```cpp
const std::size_t slot = index & (SIZE - 1);
```

The sequence number in each slot tracks how many full laps the index has made, which is what allows
producers to detect a full queue and the consumer to detect an unfinished write without any additional
state.

---

## Exception guarantees

- **`emplace`**: Provides a **Strong Exception Guarantee**. The object is constructed in the slot
  after the ticket has been claimed but before the sequence is released. If the constructor throws,
  the sequence is never updated, so the consumer will never observe the slot as ready. However,
  because the ticket has already been claimed via `fetch_add`, **the slot will be permanently
  lost** and the effective capacity reduced by one. Only use `emplace` with types whose constructors
  do not throw.

- **`try_emplace`**: Provides a **Strong Exception Guarantee**. Construction happens after the CAS
  succeeds. If the constructor throws, the slot is permanently lost for the same reason as above.

- **`try_pop`**: Provides a **No-Throw Guarantee**. By enforcing `std::is_nothrow_move_constructible_v<T>`,
  the class guarantees that pulling an item out of the queue will never throw, preventing irrecoverable
  states where an item is logically removed from the queue but fails to be delivered to the consumer.

---

## Performance characteristics

Check [benchmarks](../../benchmarks)

---

## Limitations

- **Fixed Size**: Capacity is fixed at compile time and cannot grow dynamically.

- **MPSC Only**: Multiple consumers calling `try_pop()` concurrently will break the memory model and
  cause data races. For multi-consumer use cases, refer to the MPMC structure.

- **`emplace` can block indefinitely**: If producers outpace the consumer and the queue fills up,
  threads calling `emplace()` will spin forever. Use `try_emplace()` when blocking is unacceptable.

- **Slot leak on constructor throw**: If `T`'s constructor throws inside `emplace` or `try_emplace`,
  the claimed slot is permanently lost, silently reducing capacity. Prefer nothrow-constructible types.

- **Move Semantics Required**: Objects must be nothrow move constructible. Legacy types that can only
  be copied or that throw on move cannot be used with this queue.

---

## Testing and validation

Check [tests](../../tests)

---

## Example

Check [examples](../../examples)