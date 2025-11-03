// Concurrent_Stack_LF_Ring_Ticket_MPMC.hpp
//
// Description:
//   The ticket-based solution for the lock-free/ring/MPMC stack problem:
//     Limits the shared data usage to the monotonic top counter
//     by forcing each thread to work on different slots of the static buffer.
//     Hence, the synchronization is limitted to the atomic top counter.
//     The result is achieved by defining a ticket for each slot.
//     A thread may operate on a slot if the ticket of the slot macthes the top counter.
//
// Requirements:
// - capacity must be a power of two (for fast masking).
// - T must be noexcept-movable.
//
// Semantics:
//   push():
//     Reserves a unique top_ticket via fetch_add on the monotonic top counter.
//     The expected slot ticket for that top_ticket is:
//       slot.expected_ticket = top_ticket & MASK
//     where MASK is the bit masker that allows cycling the buffer.
//     Producer waits until: slot.expected_ticket == top_ticket
//     meaning that the slot is EMPTY for that cycle.
//     When the wait condition is achieved,
//     constructs T in-place and publishes the data
//     by marking the slot as FULL: expected_ticket = top_ticket + 1
//     meaning that the slot is ready for a pop.
//   pop():
//     Grabs the most-recent top_ticket by a CAS-decrement operation on the top counter.
//     That yields top_ticket = old_top - 1 which ensures LIFO.
//     Consumer waits until: slot.expected_ticket == top_ticket + 1
//     meaning that the slot is FULL for that cycle.
//     When the wait condition is achieved,
//     moves the data, destroys it and
//     marks the slot EMPTY: slot.expected_ticket = top_ticket + capacity
//     meaning that for the next round the slot will be ready for a push.
//
//   Try configurations (tyr_push and try_pop):
//     Producers and consumers need to wait
//     for the corresponding ticket conditions explained above.
//     In try configuration, the threads do not wait but
//     tries to execute the operation immediately.
//     The operation is performed only if the ticket conditioned is satisfied.
//
// Progress:
//   Lock-free:
//     A stalled thread can delay a specific slot but
//     does not block others from operating on other slots.
//   push():
//     back-pressures when the stack is full by spinning on its reserved slot.
//   pop():
//     returns empty immediately if it cannot reserve a top_ticket (_top == 0).
//
// Notes:
//   No dynamic allocation or reclamation:
//     ABA is avoided by slot.expected_ticket.
//   Memory orders are chosen to release data before
//   the visibility of FULL and to acquire data after observing FULL.
//
// CAUTION:
//   use stack_LF_ring_ticket_MPMC alias at the end of this file
//   to get the right specialization of Concurrent_Stack
//   and to achieve the default arguments consistently.
//
// CAUTION:
//   Threads may spin indefinitely if a counterpart thread fails mid-operation,
//   as by design, each operation (e.g. producer)
//   works on a unique slot and
//   waits for the counterpart (e.g. consumer)
//   to finish its job (e.g. pop the value and adjust the ticket to imply EMPTY state).
//   So, the producer will fall in an infinite loop, if the counter consumer fails/stalls.
//   Or, the consumer will fall in an infinite loop, if the counter producer fails/stalls.
//
// See Concurrent_Stack_LF_Ring_Ticket_MPMC for ticket-based version
// which guarantees lock-free execution regardless of the contention.

#ifndef CONCURRENT_STACK_LF_RING_TICKET_MPMC_HPP
#define CONCURRENT_STACK_LF_RING_TICKET_MPMC_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <new>
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
            char pad[64 - sizeof(_expected_ticket) - sizeof(_data) % 64]; // explicit alignment to ensure 64
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
        Concurrent_Stack() noexcept {
            for (std::uint64_t i = 0; i < capacity; ++i) {
                _slots[i]._expected_ticket.store(i, std::memory_order_relaxed);
            }
        }

        // Assume single-threaded destruction.
        // Remove the remaining (not-yet-popped) elements which satisfy:
        //   slot._expected_ticket == top_ticket + 1
        ~Concurrent_Stack() {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                for (std::uint64_t i = 0; i < capacity; ++i) {
                    auto expected_ticket = _slots[i]._expected_ticket.load(std::memory_order_relaxed);
                    if (((expected_ticket - 1) & _MASK) == i) _slots[i].to_ptr()->~T();
                }
            }
        }

        // Non-copyable / non-movable for simplicity
        Concurrent_Stack(const Concurrent_Stack&) = delete;
        Concurrent_Stack& operator=(const Concurrent_Stack&) = delete;

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
            ::new (slot.to_ptr()) U(std::forward<U>(data));
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
            ::new (slot.to_ptr()) U(std::forward<U>(data));
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
            const std::uint64_t old_top = _top.load(1, std::memory_order_acquire);
            if (
                !_top.compare_exchange_strong(
                    old_top,
                    old_top - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) return std::nullopt;

            const std::uint64_t top_ticket = old_top - 1;
            const std::size_t slot_index = static_cast<std::size_t>(top_ticket & _MASK);
            Slot& slot = _slots[slot_index];

            // try if the slot is currently empty (slot._expected_ticket == top_ticket + 1) for this top_ticket.
            if (slot._expected_ticket.load(std::memory_order_acquire) != top_ticket + 1)
                return std::nullopt;

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
