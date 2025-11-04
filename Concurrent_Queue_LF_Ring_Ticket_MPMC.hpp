// Concurrent_Queue_LF_Ring_Brute_Force_MPMC.hpp
//
// Description:
//   The brute force solution for the lock-free/ring/MPMC queue problem:
//     Synchronize the tail of the static ring buffer 
//       which is shared between producer and consumer threads.
//     Define an atomic state per buffer slot
//       which synchronizes the producer-consumer pair working on the same slot.
//
// Requirements:
// - _CAPACITY must be a power of two (for fast masking).
// - T must be noexcept-movable.
//
// Invariant:
//   No fragmentation within the buffer is allowed.
//   data shall be continuously stored into the buffer
//   and popped without creating gaps.
//
// Semantics:
//   Slot class:
//     The ring buffer is a contiguous array of Slot objects.
//     A slot encapsulattes two members:
//       1. The data of type T
//       2. A state flag indicating if the slot is ready to use.
//
//   Slot states:
//     4 possible states:
//       1. POP_PROGRESS
//       2. POP_DONE (initial state of a slot)
//       3. PUSH_PROGRESS
//       4. PUSH_DONE
//
//   A pair of producer and consumer threads shares the ownership of a slot
//   which is shown clearly in the following pseudocodes of push and pop.
//
//   push():
//     1. Try to own the tail slot: CAS loop to increment it:
//        while(!_tail.CAS(tail, tail + 1,...) { tail = _tail.load(...); }
//     2. Sharing the ownership of the tail slot with a consumer: CAS loop to POP_DONE -> PUSH_PROGRESS:
//        _slots[tail]._state.CAS(POP_DONE, PUSH_PROGRESS,...);
//     3. The slot is owned now. push the data:
//        _slots[tail]._data = std::move(data);
//     4. Store the state as PUSH_DONE:
//        _slots[tail]._state.store(PUSH_DONE,...);
//   pop():
//     1. Try to own the tail slot: CAS loop to decrement it:
//        while(!_tail.CAS(tail, tail - 1,...) { tail = _tail.load(...); }
//     2. Sharing the ownership of the [tail - 1] slot with a producer: CAS loop to PUSH_DONE -> POP_PROGRESS:
//        _slots[tail - 1]._state.CAS(PUSH_DONE, POP_PROGRESS,...);
//     3. The slot is owned now. pop the data:
//        auto data = std::move(_slots[tail - 1]._data);
//     4. Store the state as POP_DONE:
//        _slots[tail - 1]._state.store(POP_DONE,...);
//     5. return the popped data:
//        return data;
//
//   This algorithm is completely serialized on the atomic tail.
//   All threads will suffer from starvation.
//
// Progress:
//   Lock-free:
//     NOT EVEN OBSTRUCTION-FREE!
//
// Notes:
//   Memory orders are chosen to
//   release data before the visibility of the state transitions and
//   to acquire data after observing the state transitions.
//
// CAUTION:
//   ABA remains unsolved.
//   A thread having a state data stale from the previous round of the ring
//   may interfere with the thread working on the same slot for the current round.
//
// CAUTION:
//   This is a simple conceptual model for a lock-free/ring-buffer/MPMC queue problem
//   but actually not lock-free; even worst not obstruction-free
//   as all threads are serialized on the atomic tail.
//   Actually, this is one-to-one conversion of a single thread queue to a concurrent one:
//     a push increments the tail index and a pop decrements it
//     where the synchronization for the tail index is handled by a state flag.
//
// CAUTION:
//   This model does not allow optimizations for simple producer/consumer cases.
//   Even under SPSC configuration the same synchronization model is required
//   as all threads serialize on the atomic tail.
//   Hence, MPSC, SPMC and SPSC configurations use the same alias:
//     queue_LF_ring_brute_force_MPSC = queue_LF_ring_brute_force_MPMC
//     queue_LF_ring_brute_force_SPMC = queue_LF_ring_brute_force_MPMC
//     queue_LF_ring_brute_force_SPSC = queue_LF_ring_brute_force_MPMC
//   See the end of this header for the alias definitiions.
//
// CAUTION:
//   use queue_LF_ring_brute_force_MPMC alias at the end of this file
//   to get the right specialization of Concurrent_Queue
//   and to achieve the default arguments consistently.
//
// CAUTION:
//   See Concurrent_Queue_LF_Ring_Ticket_MPMC for ticket-based version
//   which guarantees lock-free execution.

#ifndef CONCURRENT_QUEUE_LF_RING_BRUTE_FORCE_MPMC_HPP
#define CONCURRENT_QUEUE_LF_RING_BRUTE_FORCE_MPMC_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include "Concurrent_Queue.hpp"
#include "enum_ring_designs.hpp"
#include "aux_type_traits.hpp"

namespace BA_Concurrency {
    // use queue_LF_ring_brute_force_MPMC alias at the end of this file
    // to get the right specialization of Concurrent_Queue
    // and to achieve the default arguments consistently.
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    requires ( // for the thread safety of pop as it returns std::optional<T>
            std::is_nothrow_move_constructible_v<T> &&
            std::is_nothrow_move_assignable_v<T>)
    class Concurrent_Queue<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Brute_Force)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
    {
        static constexpr std::size_t _CAPACITY = pow2_size<Capacity_As_Pow2>;
        static constexpr std::size_t _MASK     = _CAPACITY - 1;

        struct alignas(64) Slot {
            std::atomic<std::size_t> _expected_ticket;
            alignas(T) unsigned char _data[sizeof(T)];
            T* to_ptr() noexcept { return std::launder(reinterpret_cast<T*>(_data)); }
        };

        alignas(64) std::atomic<std::size_t> _head{0};  // next ticket to pop
        alignas(64) std::atomic<std::size_t> _tail{0};  // next ticket to push
        Slot _slots[_CAPACITY];

    public:

        // Initialize each slot to expect its index as the first producer ticket
        Concurrent_Queue() noexcept {
            for (std::size_t i = 0; i < _CAPACITY; ++i) {
                _slots[i]._expected_ticket.store(i, std::memory_order_relaxed); // exoected = producer ticket
            }
        }

        // Single-threaded context expected.
        // destroy the elements that were enqueued but not yet dequeued
        ~Concurrent_Queue() {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                const std::size_t consumer_ticket = _head.load(std::memory_order_relaxed);
                const std::size_t producer_ticket = _tail.load(std::memory_order_relaxed);
                for (std::size_t ticket = consumer_ticket; ticket < producer_ticket; ++p) {
                    auto& slot = _slots[ticket & _MASK];
                    if (slot._expected_ticket.load(std::memory_order_relaxed) == ticket + 1) {
                        slot.to_ptr()->~T();
                    }
                }
            }
        }

        Concurrent_Queue(const Concurrent_Queue&) = delete;
        Concurrent_Queue& operator=(const Concurrent_Queue&) = delete;

        // Blocking enqueue: busy-wait while FULL at reservation time.
        //
        // Operation steps:
        //   1. Fetch increment the tail to obtain the producer ticket:
        //      const std::size_t producer_ticket = _tail.fetch_add(1, std::memory_order_acq_rel);
        //   2. Wait until the slot expects the obtained producer ticket:
        //      while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);
        //   3. The slot is owned now. push the data:
        //      ::new (slot.to_ptr()) T(std::forward<U>(data));
        //   4. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
        //      Notice that, the FULL condition will happen
        //      when the consumer ticket reaches the current producer ticket:
        //      slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);
        //
        // Notes:
        //   1. If there exists a single producer (SPMC or SPSC), the tail index
        //      does not need to be an atomic.
        template <class U>
        void push(U&& data) noexcept {
            // Step 1
            const std::size_t producer_ticket = _tail.fetch_add(1, std::memory_order_acq_rel);
            Slot& slot = _slots[producer_ticket & _MASK];

            // Step 2
            while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);

            // Step 3
            ::new (slot.to_ptr()) T(std::forward<U>(data));

            // Step 4
            slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);
        }

        // Blocking dequeue: busy-wait while EMPTY at reservation time.
        //
        // Operation steps:
        //   1. Fetch increment the head to obtain the consumer ticket:
        //      std::size_t consumer_ticket = _head.fetch_add(1, std::memory_order_acq_rel);
        //   2. Wait until the slot expects the obtained consumer ticket:
        //      while (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1);
        //   3. The slot is owned now. pop the data:
        //      T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
        //   4. If not trivially destructible, call the T's destructor:
        //      if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
        //   5. Mark the slot as EMPTY (expected_ticket = producer_ticket)
        //      Notice that, the EMPTY condition will happen
        //      when the producer reaches the next round of this slot:
        //      slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
        //   6. return data;
        //
        // Notes:
        //   1. If there exists a single producer (SPMC or SPSC), the head index
        //      does not need to be an atomic.
        std::optional<T> pop() noexcept {
            // Step 1
            std::size_t consumer_ticket = _head.fetch_add(1, std::memory_order_acq_rel);
            Slot& slot = _slots[consumer_ticket & _MASK];

            // Step 2
            while (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1);

            // Step 3
            T* ptr = slot.to_ptr();
            std::optional<T> data{ std::move(*ptr) };

            // Step 4
            if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();

            // Step 5
            slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);

            // Step 6
            return data;
        }

        // Non-blocking enqueue: Returns false if FULL at reservation time.
        //
        // Operation steps:
        //   1. Fetch increment the tail to obtain the producer ticket:
        //      const std::size_t producer_ticket = _tail.fetch_add(1, std::memory_order_acq_rel);
        //   2. Inspect if the ring is FULL for this producer ticket:
        //      if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket) return false;
        //   3. The slot is owned now. push the data:
        //      ::new (slot.to_ptr()) T(std::forward<U>(data));
        //   4. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
        //      Notice that, the FULL condition will happen
        //      when the consumer ticket reaches the current producer ticket:
        //      slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);
        //   5. return true;
        //
        // Notes:
        //   1. If there exists a single producer (SPMC or SPSC), the tail index
        //      does not need to be an atomic.
        template <class U>
        bool try_push(U&& data) noexcept {
            // Step 1
            const std::size_t producer_ticket = _tail.fetch_add(1, std::memory_order_acq_rel);
            Slot& slot = _slots[producer_ticket & _MASK];

            // Step 2
            if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket)
                return false;

            // Step 3
            ::new (slot.to_ptr()) T(std::forward<U>(data));

            // Step 4
            slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);

            // Step 5
            return true;
        }

        // Non-blocking dequeue: Returns nullopt if EMPTY at reservation time.
        //
        // Operation steps:
        //   1. Load the head:
        //      std::size_t head = _head.load(std::memory_order_acquire);
        //   2. Inspect if the queue is empty:
        //      if (head == _tail.load(std::memory_order_acquire)) return std::nullopt;
        //   3. CAS the head to get the ownership of the slot:
        //      if (head == _tail.load(std::memory_order_acquire)) return std::nullopt;
        //   4. Inspect if the slot expects the head:
        //      if (slot._expected_ticket.load(std::memory_order_acquire) != head + 1) return std::nullopt;
        //   5. Pop the data:
        //      T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
        //   6. If not trivially destructible, call the T's destructor:
        //      if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
        //   7. Mark the slot as EMPTY (expected_ticket = producer_ticket)
        //      Notice that, the EMPTY condition will happen
        //      when the producer reaches the next round of this slot:
        //      slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
        //   8. return data;
        //
        // Notes:
        //   1. Step 2 yields coupling with producer threads via _tail member.
        //      _tail is used to detect the empty queue situation
        //      earlier with a cheap operation (i.e. atomic load)
        //      before using costly CAS operations.
        //      Its not mandatory but an optimization.
        //      SPMC and SPSC configurations would exclude the _tail use in try_pop method
        //      in order to separate the producers and consumers completely
        //      and to optimize the two tickets: _head and _tail.
        //      This will cause a little performance loss in case of empty queues
        //      for SPMC and SPSC but let the optimization be more effective for the other functions.
        //      For example, SPSC configuration may define _head and _tail
        //      with non-atomic types.
        std::optional<T> try_pop() noexcept {
            // Step 1
            std::size_t head = _head.load(std::memory_order_acquire);

            while (true) {
                // Step 2
                if (head == _tail.load(std::memory_order_acquire))
                    return std::nullopt;

                // Step 3
                if (_head.compare_exchange_weak(
                        head,
                        head + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    Slot& slot = _slots[head & _MASK];

                    // Step 4
                    if (slot._expected_ticket.load(std::memory_order_acquire) != head + 1)
                        return std::nullopt;

                    // Step 5
                    T* ptr = slot.to_ptr();
                    std::optional<T> data{std::move(*ptr)};

                    // Step 6
                    if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();

                    // Step 7
                    slot._expected_ticket.store(head + _CAPACITY, std::memory_order_release);

                    // Step 8
                    return data;
                }
            }
        }

        bool empty() const noexcept {
            return
                _head.load(std::memory_order_acquire) ==
                _tail.load(std::memory_order_acquire);
        }

        std::size_t capacity() const noexcept { return _CAPACITY; }
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using queue_LF_ring_brute_force_MPMC = Concurrent_Queue<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Brute_Force)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
    
    // As all threads serialize on the atomic tail,
    // MPSC, SPMC and SPSC are the same as MPMC configuration.
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using queue_LF_ring_brute_force_MPSC = queue_LF_ring_brute_force_MPMC<
        T,
        Capacity_As_Pow2>;
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using queue_LF_ring_brute_force_SPMC = queue_LF_ring_brute_force_MPMC<
        T,
        Capacity_As_Pow2>;
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using queue_LF_ring_brute_force_SPSC = queue_LF_ring_brute_force_MPMC<
        T,
        Capacity_As_Pow2>;
} // namespace BA_Concurrency

#endif // CONCURRENT_QUEUE_LF_RING_BRUTE_FORCE_MPMC_HPP
