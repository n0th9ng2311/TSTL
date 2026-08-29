# lockfree::SPSC
`tstl::lockfree::SPSC` is a bounded, non-blocking Single-Producer Single-Consumer (SPSC) queue. Its
operations complete in a bounded number of steps and return immediately when the queue is full or empty.
---

## Overview
`tstl::lockfree::SPSC` is a high-performance, bounded, wait-free Single-Producer Single-Consumer (SPSC) queue.
It is designed for ultra-low latency inter-thread communication.

The data structure uses a ring buffer backed by inline uninitialized storage, preventing the overhead of 
default construction. It heavily leverages cache line alignment and local index caching to eliminate false 
sharing and reduce cache coherence traffic over the interconnect.

---

## Requirements
- `T` Requirements: T must satisfy `std::is_nothrow_move_constructible_v<T>`. This guarantees that moving objects
out of the queue during a pop operation will not throw.

- `SIZE` Requirements: SIZE must be a power of 2.

---

## Threading contract
This class enforces a strict **Single-Producer, Single-Consumer** concurrency model:

- **Producer Thread**: Only one specific thread may call `try_emplace()` at any given time.

- **Consumer Thread**: Only one specific thread may call `try_pop()` at any given time.

**Violation**: Concurrent calls to `try_emplace` by multiple threads, or `try_pop` by multiple threads, 
will result in data races, memory corruption, and undefined behavior.

---

## API reference

### Constructor and destructor

```c++
SPSC() = default;
~SPSC();
```
- `SPSC()`: Initializes the queue in an empty state. Indices are zero-initialized.

- `~SPSC()`: Drains the queue by repeatedly calling try_pop() until empty.
  Destruction is not thread-safe. The producer and consumer must have stopped using the queue before its destructor runs.

- **Copy/Move Semantics**: The queue is strictly non-copyable and non-movable.

### try_emplace
```c++
template<typename... Args>
[[nodiscard]] bool try_emplace(Args &&...args);
```
Attempts to construct an element of type `T` in-place at the current write head.

- **Parameters**: `args...` - Arguments forwarded to T's constructor.

- **Returns**: true if the element was successfully enqueued. false if the queue is full.

- **Performance**: Wait-free. Uses a cached read head to avoid atomic loads on the consumer's index on every call.

### try_pop
```c++
[[nodiscard]] std::optional<T> try_pop();
```
Attempts to extract the next element from the queue.

- **Returns**: A `std::optional<T>` containing the extracted element if the queue was not empty, or 
`std::nullopt` if the queue was empty.

- **Behavior**: The item is move-constructed into the returning optional, and explicitly destroyed 
in the queue's memory slot.

- **Performance**: Wait-free. Uses a cached write head to avoid atomic loads on the producer's index 
on every call.

### capacity
```c++
[[nodiscard]] static constexpr std::size_t capacity() noexcept;
```
- **Returns**: The fixed maximum number of elements the queue can hold, which is equal to the `SIZE` 
template parameter.

## Memory-ordering design
1) **Local Head Access** (`memory_order_relaxed`): The producer owns the write_head and the consumer owns the read_head.
Reading their own heads is done with relaxed memory order because no other thread modifies them.

2) Cross-Thread Synchronization (`acquire` / `release`):

- When `try_emplace` successfully writes to a slot, it updates `write_head` using `memory_order_release`.

- When `try_pop` fails to find new items using its cached write index, it reloads `write_head` using `memory_order_acquire`.

- This acquire-release pair guarantees that the payload constructed by the producer is fully visible to 
the consumer before the index update is observed. The exact same inverted logic applies to the 
`read_head` to prevent the producer from overwriting unread slots.

3) **False Sharing Prevention**: `write_head` and `read_head` (along with their cached counterparts) are explicitly 
aligned to `detail::CACHE_LINE_SIZE`. This ensures they sit on separate hardware cache lines.

---

## Object lifetime
the queue avoids default-constructing `SIZE` elements at initialization.
Instead, it uses an internal `Slot` structure containing a raw `std::byte` array aligned to `alignas(T)`.

- **Creation**: `std::construct_at` is used inside `try_emplace` to invoke the constructor directly in the raw memory.

- **Destruction**: `std::destroy_at` is called inside `try_pop` immediately after the object's contents have been moved out.

- **Cleanup**: The destructor guarantees that any remaining items are popped (and therefore explicitly destroyed) upon queue destruction.

---

## Capacity and wraparound

The queue acts as a ring buffer utilizing a continuously incrementing index that is masked to the bounded capacity.\
Because `SIZE` is validated via static_assert to be a power of 2, the buffer translates absolute indices to ring buffer 
slots using bitwise masking:
```c++
const std::size_t slot = current_index & (SIZE - 1);
```
---

## Exception guarantees

- `try_emplace`: Provides a Strong Exception Guarantee. The constructor of T is invoked via `std::construct_at` 
*before* the atomic `write_head` is updated. If the constructor throws an exception, the state of the queue
remains entirely unmodified, and the slot is safely abandoned.

- `try_pop`: Provides a **No-Throw Guarantee**. By enforcing `std::is_nothrow_move_constructible_v<T>`, the class guarantees
that pulling an item out of the queue will never throw, preventing irrecoverable states where an item is logically removed
from the queue but fails to be delivered to the consumer.

---

## Performance characteristics
Check [benchmarks](../../benchmarks)

---

## Limitations

- **Fixed Size**: The capacity is fixed at compile-time and cannot dynamically grow.

- **SPSC Only**: Attempting to use this in a Multi-Producer or Multi-Consumer context
will break the memory model and cause data races.(we have other structures you might want
to look into if you need multi consumer and producer)

- **Move Semantics Required**: Objects must be nothrow move constructible. Legacy types that can only
be copied or that throw on move cannot be used in this queue.

---

## Testing and validation

Check [tests](../../tests)

---

## Example
Check [examples](../../examples)
