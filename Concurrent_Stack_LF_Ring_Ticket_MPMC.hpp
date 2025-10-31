// Concurrent_Stack_LF_Ring_Ticket_MPMC
//
// Description:
//   The ticket solution for the lock-free/MPMC/ring stack problem:
//     Synchronize the top of the static ring buffer 
//       which is shared between producer and consumer threads.
//     Define an atomic state per buffer slot
//       which synchronizes the producers and consumers.
//
// Requirements:
// - capacity must be a power of two (for fast masking).
// - T must be noexcept-movable.
//
// CAUTION:
//   This is a simple conceptual model for a lock-free/ring-buffer/MPMC stack problem
//   but actually not fully lock-free under heavy contention (i.e. obstruction-free)
//   as the single atomic top synchronization allows
//   each thread to execute only in isolation (i.e. no contention).
//   Actually, this is one-to-one conversion of a single thread queue to a concurrent one:
//     a push increments the top index and a pop decrements it.
//     wheree the synchronization for the top index is handled by a state flag.
//
// CAUTION:
//   In order to reduce the collision probability,
//   the capacity of the buffer shall be increased.
//   Amprically, the following equality results well to achieve a lock-free execution:
//     capacity = 8 * N where N is the number of the threads
//
// CAUTION:
//   use stack_LF_ring_ticket_MPMC alias at the end of this file
//   to get the right specialization of Concurrent_Stack
//   and to achieve the default arguments consistently.
//
// Atomic slot states where PT and CT stand for producer and consumer threads respectively: 
//   SPP: State-Producer-Progress: PT owns the slot and is operating on it
//   SPW: State-Producer-Waiting : PT shares the slot ownership with a CT and waiting for the CT's notify
//   SPD: State-Producer-Done    : PT released the slot ownership after storing the data
//   SPR: State-Producer-Ready   : PT released the slot ownership to the waiting CT (notify CT) after storing the data
//   SCP: State-Consumer-Progress: CT owns the slot and is operating on it
//   SCW: State-Consumer-Waiting : CT shares the slot ownership with a PT and waiting for the PT's notify
//   SCD: State-Consumer-Done    : CT released the slot ownership after popping the data
//   SCR: State-Consumer-Ready   : CT released the slot ownership to the waiting PT (notify PT) after popping the data
//
// Example state transitions for a slot:
//   SCD->SPP->SPD->SCP->SCD:
//     no interference by counter threads while this thread is in progress (i.e. SPP and SCP)
//   SCD->SPP->SCW->SPR->SCP->SPW->SCR->SPP:
//     a counter thread interferes and starts waiting during this thread is in progress (i.e. SPP and SCP)
//
// See Concurrent_Stack_LF_Ring_Ticket_MPMC for ticket-based version
// which guarantees lock-free execution regardless of the contention.


// Concurrent_Stack_LF_Ring_Ticket_MPMC
//
// TODO: see the documentation of Concurrent_Stack_LF_Ring_Brute_Force_MPMC
//
// Stack: bounded lock-free MPMC stack over a contiguous ring buffer.
//
// Requirements:
// - capacity must be a power of two (for fast masking).
// - T must be MoveConstructible, and ideally noexcept-movable for best guarantees.
//
// Semantics:
// - push():
//     reserves a unique "top_ticket" via fetch_add on _top (monotonic).
//     The slot for that top_ticket is (top_ticket & mask). Producer waits until the
//     slot's expected_ticket equals top_ticket (meaning it's empty for this cycle),
//     constructs T in-place, then publishes by setting expected_ticket = top_ticket + 1.
// - pop():
//     grabs the most-recent top_ticket by CAS-decrementing _top. That yields
//     top_ticket = old_top - 1 (LIFO). Consumer waits until the slot's expected_ticket equals
//     top_ticket + 1 (meaning full), moves the data, destroys it, then marks the
//     slot empty for the *next* wraparound by setting expected_ticket = top_ticket + capacity.
//
// Progress:
// - Lock-free:
//     A stalled thread can delay a specific slot but does
//     not block others from operating on other slots.
// - push():
//     back-pressures when the stack is full by spinning on its reserved slot.
// - pop():
//     returns empty immediately if it cannot reserve a top_ticket (_top == 0).
//
// Notes:
// - No dynamic allocation or reclamation:
//     ABA is avoided by per-slot expected_ticket.
// - Memory orders chosen to release data before visibility of "full" and to acquire data after observing "full".

#ifndef CONCURRENT_STACK_LF_RING_TICKET_MPMC_HPP
#define CONCURRENT_STACK_LF_RING_TICKET_MPMC_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <new>
#include <type_traits>
#include <optional>
#include "Concurrent_Stack.hpp"
#include "enum_ring_designs.hpp"

namespace BA_Concurrency {
    template <unsigned char power>
    struct pow2_size_t {
        static constexpr std::size_t value = std::size_t(1) << power;
    };
    
    // use stack_LF_ring_ticket_MPMC alias at the end of this file
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
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Ticket)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
    {
        struct alignas(64) Slot {
            std::atomic<std::uint64_t> _expected_ticket; // will be initialized during the stack's constructor
            alignas(T) unsigned char _data[sizeof(T)];
            T* to_ptr() noexcept { return std::launder(reinterpret_cast<T*>(_data)); }
        };

        // mask to modulo the top_ticket by capacity
        static constexpr std::size_t capacity = std::size_t(1) << Capacity_As_Pow2;
        static constexpr std::size_t _MASK = capacity - 1;

        // _top is the next top_ticket to push.
        alignas(64) std::atomic<std::uint64_t> _top{0};
        // the buffer of slots
        Slot _slots[capacity];

    public:

        // Initialize the buffer indices so that
        // the very first producer that reserves top_ticket will satisfy:
        //   slot._expected_ticket == top_ticket
        Stack() noexcept {
            for (std::uint64_t i = 0; i < capacity; ++i) {
                _slots[i]._expected_ticket.store(i, std::memory_order_relaxed);
            }
        }

        // Assume single-threaded destruction.
        // Remove the remaining (not-yet-popped) elements which satisfy:
        //   slot._expected_ticket == top_ticket + 1
        ~Stack() {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (std::uint64_t i = 0; i < capacity; ++i) {
                    auto expected_ticket = _slots[i]._expected_ticket.load(std::memory_order_relaxed);
                    if (((expected_ticket - 1) & _MASK) == i) _slots[i].to_ptr()->~T();
                }
            }
        }

        // Non-copyable / non-movable for simplicity
        Stack(const Stack&) = delete;
        Stack& operator=(const Stack&) = delete;

        // blocking (busy) push
        template <class U>
        void push(U&& data) noexcept {
            // reserve a unique ticket for the top
            const std::uint64_t top_ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t slot_index = static_cast<std::size_t>(top_ticket & _MASK);
            Slot& slot = _slots[slot_index];

            // Spin until this slot is empty for this top_ticket (expected_ticket == top_ticket)
            while (slot._expected_ticket.load(std::memory_order_acquire) != top_ticket);

            // construct and publish
            ::new (slot.to_ptr()) T(std::forward<U>(data));
            slot._expected_ticket.store(top_ticket + 1, std::memory_order_release);
        }

        // blocking (busy) emplace
        template <typename... Args>
        void emplace(Args&&... args) {
            // reserve a unique ticket for the top
            const std::uint64_t top_ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t slot_index = static_cast<std::size_t>(top_ticket & _MASK);
            Slot& slot = _slots[slot_index];

            // Spin until this slot is empty for this top_ticket (expected_ticket == top_ticket)
            while (slot._expected_ticket.load(std::memory_order_acquire) != top_ticket);

            // construct and publish
            ::new (slot.to_ptr()) T(std::forward<Args>(args)...);
            slot._expected_ticket.store(top_ticket + 1, std::memory_order_release);
        }


        // blocking (busy) pop
        // Pop the most recently pushed element (LIFO) after the producer finishes the publish.
        // Returns std::nullopt if stack appears empty at the reservation time.
        std::optional<T> pop() noexcept {
            // Reserve the latest top_ticket by CAS-decrementing _top.
            // If _top == 0, stack is empty.
            std::uint64_t old_top = _top.load(std::memory_order_acquire);
            while (true) {
                if (old_top == 0) return std::nullopt;

                // Try to claim top_ticket old_top-1
                if (_top.compare_exchange_strong(
                        old_top,
                        old_top - 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // the top_ticket (old_top-1) is owned by this consumer
                    const std::uint64_t top_ticket = old_top - 1;
                    const std::size_t slot_index = static_cast<std::size_t>(top_ticket & _MASK);
                    Slot& slot = _slots[slot_index];

                    // Wait until producer finishes the publish (expected_ticket == top_ticket + 1).
                    // Another consumer can't take this top_ticket because we've reserved it by CAS on _top.
                    while (slot._expected_ticket.load(std::memory_order_acquire) != top_ticket + 1);

                    // Move the data and destroy in place
                    T* ptr = slot.to_ptr();
                    std::optional<T> data{std::move(*ptr)};
                    if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();

                    // Mark slot empty for the next cycle.
                    // After a full cycle of capacity tickets, the same index will be used again.
                    // The expected_ticket of the expected empty element for that future push will be top_ticket + capacity.
                    slot._expected_ticket.store(top_ticket + capacity, std::memory_order_release);
                    return data;
                }
            }
        }

        // non-blocking (no wait) push
        template <class U>
        bool try_push(U&& data) noexcept {
            // reserve a unique ticket for the top
            const std::uint64_t top_ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t slot_index = static_cast<std::size_t>(top_ticket & _MASK);
            Slot& slot = _slots[slot_index];

            // try if the slot is currently empty (slot._expected_ticket == top_ticket) for this top_ticket.
            if (slot._expected_ticket.load(std::memory_order_acquire) != top_ticket) return false;

            // construct and publish
            ::new (slot.to_ptr()) T(std::forward<U>(data));
            slot._expected_ticket.store(top_ticket + 1, std::memory_order_release);
            return true;
        }

        // non-blocking (no wait) emplace
        template <typename... Args>
        bool try_emplace(Args&&... args) {
            // reserve a unique ticket for the top
            const std::uint64_t top_ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t slot_index = static_cast<std::size_t>(top_ticket & _MASK);
            Slot& slot = _slots[slot_index];

            // try if the slot is currently empty (slot._expected_ticket == top_ticket) for this top_ticket.
            if (slot._expected_ticket.load(std::memory_order_acquire) != top_ticket) return false;

            // construct and publish
            ::new (slot.to_ptr()) T(std::forward<Args>(args)...);
            slot._expected_ticket.store(top_ticket + 1, std::memory_order_release);
            return true;
        }

        // non-blocking (no wait) pop
        std::optional<T> try_pop() noexcept {
            // reserve a unique ticket for the top
            const std::uint64_t top_ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t slot_index = static_cast<std::size_t>(top_ticket & _MASK);
            Slot& slot = _slots[slot_index];

            // try if the slot is currently empty (slot._expected_ticket == top_ticket + 1) for this top_ticket.
            if (slot._expected_ticket.load(std::memory_order_acquire) != top_ticket + 1) return std::nullopt;

            // construct and publish
            // Move the data and destroy in place
            T* ptr = slot.to_ptr();
            std::optional<T> data{std::move(*ptr)};
            if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();

            // Mark slot empty for the next cycle.
            // After a full cycle of capacity tickets, the same index will be used again.
            // The expected_ticket of the expected empty element for that future push will be top_ticket + capacity.
            slot._expected_ticket.store(top_ticket + capacity, std::memory_order_release);
            return data;
        }
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using stack_LF_ring_ticket_MPMC = Concurrent_Stack<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Ticket)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_LF_RING_TICKET_MPMC_HPP
