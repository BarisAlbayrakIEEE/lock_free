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
// - capacity must be a power of two (for fast masking).
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
//   A thread having a state value stale from the previous round of the ring
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

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <new>
#include <optional>
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
        // Slot states
        static constexpr std::uint8_t POP_PROGRESS = 0;
        static constexpr std::uint8_t POP_DONE = 1;
        static constexpr std::uint8_t PUSH_PROGRESS = 2;
        static constexpr std::uint8_t PUSH_DONE = 3;
    
        // slot definition
        struct alignas(64) Slot {
            std::atomic<std::uint8_t> _state{ POP_DONE };
            char pad[64 - sizeof(_state)]; // padding to avoid false sharing
            T _data;
        };

        // the capacity of the buffer
        static constexpr std::size_t capacity = pow2_size<Capacity_As_Pow2>;
        // mask to modulo the head/tail by capacity to achieve the buffer index
        static constexpr std::size_t _MASK = capacity - 1;
        // the atomic head
        alignas(64) std::atomic<std::uint64_t> _head{ capacity };
        // the atomic tail
        alignas(64) std::atomic<std::uint64_t> _tail{ 0 };
        // the buffer of slots
        Slot _slots[capacity];

    public:

        // Non-copyable / non-movable for simplicity
        Concurrent_Queue(const Concurrent_Queue&) = delete;
        Concurrent_Queue& operator=(const Concurrent_Queue&) = delete;

        // Loops the slots for busy push operation
        //
        // Operation steps:
        //   1. Try to own the tail slot: CAS loop to increment it:
        //      while(!_tail.CAS(tail, tail + 1,...) { tail = _tail.load(...); }
        //   2. Sharing the ownership of the tail slot with a consumer: CAS loop to POP_DONE -> PUSH_PROGRESS:
        //      _slots[tail]._state.CAS(POP_DONE, PUSH_PROGRESS,...);
        //   3. The slot is owned now. push the data:
        //      _slots[tail]._data = std::move(data);
        //   4. Store the state as PUSH_DONE:
        //      _slots[tail]._state.store(PUSH_DONE,...);
        template <typename U = T>
        void push(U&& data) noexcept {
            // increment the tail
            std::uint64_t tail = _tail.fetch_add(1, std::memory_order_acq_rel);

            // the slot is owned (or shared with a consumer)
            Slot *slot = &_slots[tail];

            // wait until the consumer sharing the slot finishes working on the slot
            uint8_t expected_state = POP_DONE;
            while (
                !_tail.compare_exchange_weak(
                    expected_state,
                    PUSH_PROGRESS,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) expected_state = POP_DONE;

            // push the data
            slot->_data = std::move(data);

            // store slot state as DONE
            slot->_state.store(PUSH_DONE, std::memory_order_release);
        }

        // Loops the slots for busy pop operation
        //
        // Operation steps:
        //   1. Try to own the tail slot: CAS loop to decrement it:
        //      while(!_tail.CAS(tail, tail - 1,...) { tail = _tail.load(...); }
        //   2. Sharing the ownership of the [tail - 1] slot with a producer: CAS loop to PUSH_DONE -> POP_PROGRESS:
        //      _slots[tail - 1]._state.CAS(PUSH_DONE, POP_PROGRESS,...);
        //   3. The slot is owned now. pop the data:
        //      auto data = std::move(_slots[tail - 1]._data);
        //   4. Store the state as POP_DONE:
        //      _slots[tail - 1]._state.store(POP_DONE,...);
        //   5. return the popped data:
        //      return data;
        T pop() noexcept {
            // wait while the queue is empty
            std::uint64_t head = _head.load(std::memory_order_acquire);
            std::uint64_t tail = _tail.load(std::memory_order_acquire);
            while(true) {
                // inspect if empty
                while (head == tail) {
                    head = _head.load(std::memory_order_acquire);
                    tail = _tail.load(std::memory_order_acquire);
                }

                // increment the head
                if (
                    _head.compare_exchange_strong(
                        head,
                        head + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) break;
            }

            // the slot is owned (or shared with a producer)
            Slot *slot = &_slots[head & _MASK];

            // wait until the producer sharing the slot finishes working on the slot
            uint8_t expected_state = PUSH_DONE;
            while (
                !_tail.compare_exchange_weak(
                    expected_state,
                    POP_PROGRESS,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) expected_state = PUSH_DONE;

            // pop the data
            auto data = std::move(slot->_data);

            // store slot state as DONE
            slot->_state.store(POP_DONE, std::memory_order_release);

            // Step 5
            return data;
        }
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
