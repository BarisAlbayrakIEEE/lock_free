// Concurrent_Stack_LF_Ring_Brute_Force_MPMC.hpp
//
// Description:
//   The brute force solution for the lock-free/ring/MPMC stack problem:
//     Synchronize the top of the static ring buffer 
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
//   A pair of producer and consumer threads shares the ownership of a slot
//   which is shown clearly in the following pseudocodes of push and pop.
//
//   push():
//     1. Try to own the top slot: CAS loop to increment it:
//        while(!_top.CAS(top, top + 1,...) { top = _top.load(...); }
//     2. Sharing the ownership of the top slot with a consumer: wait while the state of the slot is PROGRESS:
//        _slots[top]._state.wait(PROGRESS,...);
//     3. The slot is owned now. set the state as PROGRESS and push the data:
//        _slots[top]._state.store(PROGRESS,...);
//        _slots[top]._data = std::move(data);
//     4. Store the state as DONE and notify the waiting consumer:
//        _slots[top]._state.store(DONE,...);
//        _slots[top]._state.notify_one();
//   pop():
//     1. Try to own the top slot: CAS loop to decrement it:
//        while(!_top.CAS(top, top - 1,...) { top = _top.load(...); }
//     2. Sharing the ownership of the top slot with a producer: wait while the state of the slot is PROGRESS:
//        _slots[top - 1]._state.wait(PROGRESS,...);
//     3. The slot is owned now. set the state as PROGRESS and pop the data:
//        _slots[top - 1]._state.store(PROGRESS,...);
//        auto data = std::move(_slots[top - 1]._data);
//     4. Store the state as DONE and notify the waiting producer:
//        _slots[top - 1]._state.store(DONE,...);
//        _slots[top - 1]._state.notify_one();
//     5. return the popped data:
//        return std::move(data)
//
//   This algorithm is completely serialized on the atomic top.
//   All threads will suffer from starvation.
//
// Progress:
//   Lock-free:
//     not even obstruction-free
//
// Notes:
//   No dynamic allocation or reclamation:
//     ABA is avoided by the state flag.
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
//   This is a simple conceptual model for a lock-free/ring-buffer/MPMC stack problem
//   but actually not lock-free; even worst not obstruction-free
//   as all threads are serialized on the atomic top.
//   Actually, this is one-to-one conversion of a single thread queue to a concurrent one:
//     a push increments the top index and a pop decrements it
//     where the synchronization for the top index is handled by a state flag.
//
// CAUTION:
//   This model does not allow optimizations for simple producer/consumer cases.
//   Even under SPSC configuration the same synchronization model is required
//   as all threads serialize on the atomic top.
//   Hence, MPSC, SPMC and SPSC configurations use the same alias:
//     stack_LF_ring_brute_force_MPSC = stack_LF_ring_brute_force_MPMC
//     stack_LF_ring_brute_force_SPMC = stack_LF_ring_brute_force_MPMC
//     stack_LF_ring_brute_force_SPSC = stack_LF_ring_brute_force_MPMC
//   See the end of this header for the alias definitiions.
//
// CAUTION:
//   use stack_LF_ring_brute_force_MPMC alias at the end of this file
//   to get the right specialization of Concurrent_Stack
//   and to achieve the default arguments consistently.
//
// CAUTION:
//   See Concurrent_Stack_LF_Ring_Ticket_MPMC for ticket-based version
//   which guarantees lock-free execution.

#ifndef CONCURRENT_STACK_LF_RING_BRUTE_FORCE_MPMC_HPP
#define CONCURRENT_STACK_LF_RING_BRUTE_FORCE_MPMC_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <new>
#include <optional>
#include "Concurrent_Stack.hpp"
#include "enum_ring_designs.hpp"
#include "aux_type_traits.hpp"

namespace BA_Concurrency {
    // Slot states
    // PT: producer thread
    // CT: consumer thread
    enum class Slot_States : uint8_t {
        PROGRESS,
        DONE
    };
    
    // use stack_LF_ring_brute_force_MPMC alias at the end of this file
    // to get the right specialization of Concurrent_Stack
    // and to achieve the default arguments consistently.
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    requires ( // for the thread safety of pop as it returns std::optional<T>
            std::is_nothrow_move_constructible_v<T> &&
            std::is_nothrow_move_assignable_v<T>)
    class Concurrent_Stack<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Brute_Force)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
    {
        struct alignas(64) Slot {
            std::atomic<std::uint8_t> _state{ Slot_States::DONE };
            char pad[64 - sizeof(_state)]; // padding to avoid false sharing
            T _data;
        };

        // mask to modulo the ticket by capacity
        static constexpr std::size_t capacity = pow2_size<Capacity_As_Pow2>;
        static constexpr std::size_t _MASK = capacity - 1;

        // _top is the next ticket to push.
        alignas(64) std::atomic<std::uint64_t> _top{0};
        // the buffer of slots
        Slot _slots[capacity];

    public:

        // Non-copyable / non-movable for simplicity
        Concurrent_Stack(const Concurrent_Stack&) = delete;
        Concurrent_Stack& operator=(const Concurrent_Stack&) = delete;

        // Loops the slots for busy push operation
        //
        // Operation steps:
        //   1. Try to own the top slot: CAS loop to increment it:
        //      while(!_top.CAS(top, top + 1,...) { top = _top.load(...); }
        //   2. Sharing the ownership of the top slot with a consumer: wait while the state of the slot is PROGRESS:
        //      _slots[top]._state.wait(PROGRESS,...);
        //   3. The slot is owned now. set the state as PROGRESS and push the data:
        //      _slots[top]._state.store(PROGRESS,...);
        //      _slots[top]._data = std::move(data);
        //   4. Store the state as DONE and notify the waiting consumer:
        //      _slots[top]._state.store(DONE,...);
        //      _slots[top]._state.notify_one();
        template <typename U = T>
        void push(U&& data) noexcept {
            // Step 1
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            while (
                !_top.compare_exchange_weak(
                    top,
                    top + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) top = _top.load(std::memory_order_acquire) & _MASK;

            // Sharing the ownership of the slot with a consumer
            Slot *slot = &_slots[top];

            // Step 2
            slot->_state.wait(Slot_States::PROGRESS, std::memory_order_acquire);

            // Step 3
            slot->_state.store(Slot_States::PROGRESS, std::memory_order_release);
            slot->_data = std::move(data);

            // Step 4
            slot->_state.store(Slot_States::DONE, std::memory_order_release);
            slot->_state.notify_one();
        }

        // Loops the slots for busy pop operation
        //
        // Operation steps:
        //   1. Try to own the top slot: CAS loop to decrement it:
        //      while(!_top.CAS(top, top - 1,...) { top = _top.load(...); }
        //   2. Sharing the ownership of the top slot with a producer: wait while the state of the slot is PROGRESS:
        //      _slots[top - 1]._state.wait(PROGRESS,...);
        //   3. The slot is owned now. set the state as PROGRESS and pop the data:
        //      _slots[top - 1]._state.store(PROGRESS,...);
        //      auto data = std::move(_slots[top - 1]._data);
        //   4. Store the state as DONE and notify the waiting producer:
        //      _slots[top - 1]._state.store(DONE,...);
        //      _slots[top - 1]._state.notify_one();
        //   5. return the popped data:
        //      return data;
        T pop() noexcept {
            // Step 1
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            while (
                !_top.compare_exchange_weak(
                    top,
                    top - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) top = _top.load(std::memory_order_acquire) & _MASK;

            // Sharing the ownership of the slot with a producer
            Slot *slot = &_slots[top - 1];

            // Step 2
            slot->_state.wait(Slot_States::PROGRESS, std::memory_order_acquire);

            // Step 3
            slot->_state.store(Slot_States::PROGRESS, std::memory_order_release);
            auto data = std::move(slot->_data);

            // Step 4
            slot->_state.store(Slot_States::DONE, std::memory_order_release);
            slot->_state.notify_one();

            // Step 5
            return data;
        }
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using stack_LF_ring_brute_force_MPMC = Concurrent_Stack<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Brute_Force)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
    
    // As all threads serialize on the atomic top,
    // MPSC, SPMC and SPSC are the same as MPMC configuration.
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using stack_LF_ring_brute_force_MPSC = stack_LF_ring_brute_force_MPMC<
        T,
        Capacity_As_Pow2>;
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using stack_LF_ring_brute_force_SPMC = stack_LF_ring_brute_force_MPMC<
        T,
        Capacity_As_Pow2>;
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using stack_LF_ring_brute_force_SPSC = stack_LF_ring_brute_force_MPMC<
        T,
        Capacity_As_Pow2>;
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_LF_RING_BRUTE_FORCE_MPMC_HPP
