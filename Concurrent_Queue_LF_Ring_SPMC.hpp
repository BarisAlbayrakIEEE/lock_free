// Concurrent_Queue_LF_Ring_SPMC.hpp
//
// Description:
//   The ticket-based solution for the lock-free/ring/SPMC queue problem:
//     A specialization of Concurrent_Queue_LF_Ring_MPMC.hpp
//     optimized for the SPMC configuration.
//
// Requirements:
// - T must be noexcept-constructible.
// - T must be noexcept-movable.
//
// Invariants:
//   The producer and consumers shall hold the state invariant.
//   See the definitions of _head and _tail members for the tickets
//   that allow managing the slot states.
//   The state invariants are as follows:
//     1. For a FULL slot (i.e. contains published data) the following equality shall hold:
//        slot._expected_ticket == _tail
//     2. For an EMPTY slot (i.e. does not contain data) the following equality shall hold:
//        slot._expected_ticket == consumer_ticket + 1
//
// Semantics:
//   See Concurrent_Queue_LF_Ring_MPMC.hpp for details.
//
// Optimizations Compared to MPMC:
//   1. _tail usage for the empty queue inspection in try_pop function is canceled
//      which terminates the data share between the producer and consumers.
//   2. The atomic type of _tail is replaced by a regular non-atomic one.
//
// Semantics:
//   push():
//     1. Wait until the slot expects the obtained producer ticket:
//        while (slot._expected_ticket.load(std::memory_order_acquire) != _tail);
//     2. The slot is owned now. push the data:
//        ::new (slot.to_ptr()) T(std::forward<U>(data));
//     3. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
//        Notice that, the FULL condition will be satisfied
//        when the consumer ticket reaches the current producer ticket:
//        slot._expected_ticket.store(_tail + 1, std::memory_order_release);
//     4. ++_tail;
//
//   pop():
//     1. Increment the head to obtain the consumer ticket:
//        std::size_t consumer_ticket = _head.fetch_add(1, std::memory_order_acq_rel);
//     2. Wait until the slot expects the obtained consumer ticket:
//        while (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1);
//     3. The slot is owned now. pop the data:
//        T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
//     4. If not trivially destructible, call the T's destructor:
//        if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
//     5. Mark the slot as EMPTY (expected_ticket = _tail)
//        Notice that, the EMPTY condition will be satisfied
//        when the producer reaches the next round of this consumer ticket:
//        slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
//     6. return data;
//
//   try_push(): Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
//     1. Inspect if the slot is FULL for this producer ticket:
//        if (slot._expected_ticket.load(std::memory_order_acquire) != _tail) return false;
//     2. The slot is owned now, push the data:
//        ::new (slot.to_ptr()) T(std::forward<U>(data));
//     3. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
//        Notice that, the FULL condition will be satisfied
//        when the consumer ticket reaches the current producer ticket:
//        slot._expected_ticket.store(_tail + 1, std::memory_order_release);
//     4. ++_tail;
//     5. return true;
//
//   try_pop(): Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
//     1. Load the _head to the consumer ticket:
//        std::size_t consumer_ticket = _head.load(std::memory_order_acquire);
//     2. Inspect if the queue is empty (an optimization for the empty case):
//        if (consumer_ticket == _tail.load(std::memory_order_acquire)) return std::nullopt;
//     3. Inspect if the slot is EMPTY for this consumer ticket:
//        if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1) return false;
//     4. Weak CAS the _head to get the ownership of the slot:
//        if (!_head.compare_exchange_strong(consumer_ticket, consumer_ticket + 1,...)) continue;
//     5. The slot is owned now, pop the data:
//        T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
//     6. If not trivially destructible, call the T's destructor:
//        if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
//     7. Mark the slot as EMPTY (expected_ticket = _tail)
//        Notice that, the EMPTY condition will be satisfied
//        when the producer reaches the next round of this consumer ticket:
//        slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
//     8. return data;
//
// Progress:
//   Lock-free:
//     Lock-free execution regardless of the level of contention
//     as each thread runs in isolation on its reserved slot.
//
// Notes:
//   1. Memory orders are chosen to
//      release data before the visibility of the state transitions and
//      to acquire data after observing the state transitions.
//   2. push(): Back-pressures when the queue is full by spinning on its reserved slot.
//      pop(): Back-pressures when the queue is empty by spinning on its reserved slot.
//
// Cautions:
//   1. Threads may spin indefinitely if a counterpart thread fails mid-operation,
//      before setting the expected state accordingly.
//   2. Use queue_LF_ring_SPMC alias at the end of this file
//      to get the right specialization of Concurrent_Queue
//      and to achieve the default arguments consistently.
//
// TODOs:
//   1. The blocking operations (push and pop) back-pressures
//      waiting the expected ticket of the reserved slot to satisfy ticket invariants.
//      An exponential backoff strategy is required for these blocking operations.
//   2. Similar to the 1st one, the edge cases (empty queue and full queue)
//      requires an exponential backoff strategy as well.
//      Currently, only try_pop takes the edge condition into account.

#ifndef CONCURRENT_QUEUE_LF_RING_SPMC_HPP
#define CONCURRENT_QUEUE_LF_RING_SPMC_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include "Concurrent_Queue.hpp"
#include "aux_type_traits.hpp"

namespace BA_Concurrency {
    // use queue_LF_ring_SPMC alias at the end of this file
    // to get the right specialization of Concurrent_Queue
    // and to achieve the default arguments consistently.
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    requires (
            std::is_nothrow_constructible_v<T> &&
            std::is_nothrow_move_constructible_v<T>)
    class Concurrent_Queue<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::SPMC,
        T,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
    {
        static constexpr std::size_t _CAPACITY = pow2_size<Capacity_As_Pow2>;
        static constexpr std::size_t _MASK     = _CAPACITY - 1;

        // See the class documentation in Concurrent_Queue_LF_Ring_MPMC.hpp
        // for a detailed description about the Slot class.
        struct alignas(64) Slot {
            std::atomic<std::size_t> _expected_ticket;
            alignas(T) unsigned char _data[sizeof(T)];
            T* to_ptr() noexcept { return std::launder(reinterpret_cast<T*>(_data)); }
        };

        // See the member documentation in Concurrent_Queue_LF_Ring_MPMC.hpp
        // for a detailed description about the tickets.
        // In SPMC configuration, the only difference about the ticket definition is
        // the _tail ticket being a regular non-atomic type due to the single consumer.
        alignas(64) std::atomic<std::size_t> _head{0};  // next ticket to pop
        alignas(64) std::size_t _tail{0};               // next ticket to push
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
                for (std::size_t ticket = consumer_ticket; ticket < _tail; ++p) {
                    auto& slot = _slots[ticket & _MASK];
                    if (slot._expected_ticket.load(std::memory_order_relaxed) == ticket + 1) {
                        slot.to_ptr()->~T();
                    }
                }
            }
        }

        // Non-copyable/movable for simplicity
        Concurrent_Queue(const Concurrent_Queue&) = delete;
        Concurrent_Queue& operator=(const Concurrent_Queue&) = delete;
        Concurrent_Queue(Concurrent_Queue&&) = delete;
        Concurrent_Queue& operator=(Concurrent_Queue&&) = delete;

        // Blocking enqueue: busy-wait while FULL at reservation time.
        //
        // Operation steps:
        //   1. Wait until the slot expects the obtained producer ticket:
        //      while (slot._expected_ticket.load(std::memory_order_acquire) != _tail);
        //   2. The slot is owned now. push the data:
        //      ::new (slot.to_ptr()) T(std::forward<U>(data));
        //   3. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
        //      Notice that, the FULL condition will be satisfied
        //      when the consumer ticket reaches the current producer ticket:
        //      slot._expected_ticket.store(_tail + 1, std::memory_order_release);
        //   4. ++_tail;
        //
        // Notes:
        //   1. Back-pressures when the queue is full by spinning on its reserved slot.
        //   2. The ABA problem is solved by the monotonous _tail ticket.
        //      See the definitions of FULL and EMPTY
        //      given with the decleration of _head  and _tail tickets.
        template <class U>
        void push(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            // Step 1
            Slot& slot = _slots[_tail & _MASK];
            while (slot._expected_ticket.load(std::memory_order_acquire) != _tail);

            // Step 2
            ::new (slot.to_ptr()) T(std::forward<U>(data));

            // Step 3
            slot._expected_ticket.store(_tail + 1, std::memory_order_release);

            // Step 4
            ++_tail;
        }

        // Blocking dequeue: busy-wait while EMPTY at reservation time.
        //
        // Operation steps:
        //   1. Increment the head to obtain the consumer ticket:
        //      std::size_t consumer_ticket = _head.fetch_add(1, std::memory_order_acq_rel);
        //   2. Wait until the slot expects the obtained consumer ticket:
        //      while (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1);
        //   3. The slot is owned now. pop the data:
        //      T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
        //   4. If not trivially destructible, call the T's destructor:
        //      if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
        //   5. Mark the slot as EMPTY (expected_ticket = _tail)
        //      Notice that, the EMPTY condition will be satisfied
        //      when the producer reaches the next round of this consumer ticket:
        //      slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
        //   6. return data;
        //
        // Notes:
        //   1. Back-pressures when the queue is empty by spinning on its reserved slot.
        //   2. If stalls, only its reserved slot delays
        //      but does not block others from operating on the other slots.
        //   3. The ABA problem is solved by the monotonous _head ticket.
        //      See the definitions of FULL and EMPTY
        //      given with the definition of _head and _tail members.
        std::optional<T> pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
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
        // The difference with the blocking push is that
        // the non-blocking version does not request the slot ownership
        // until its proven that the slot satisfies the push condition
        // and the operation can be performed (i.e. the trial is going to succeed).
        // This design is mandatory for the non-blocking operations
        // as the non-blocking operations contracts not to modify the state of the queue
        // in case of failure (i.e. the trial has failed).
        //
        // Operation steps: The infinite loop of MPMC is removed as the CAS is removed:
        //   1. Inspect if the slot is FULL for this producer ticket:
        //      if (slot._expected_ticket.load(std::memory_order_acquire) != _tail) return false;
        //   2. The slot is owned now, push the data:
        //      ::new (slot.to_ptr()) T(std::forward<U>(data));
        //   3. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
        //      Notice that, the FULL condition will be satisfied
        //      when the consumer ticket reaches the current producer ticket:
        //      slot._expected_ticket.store(_tail + 1, std::memory_order_release);
        //   4. ++_tail;
        //   5. return true;
        //
        // Notes:
        //   1. The ABA problem is solved by the monotonous _tail ticket.
        //      See the definitions of FULL and EMPTY
        //      given with the decleration of _head  and _tail tickets.
        template <class U>
        bool try_push(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            // Step 1
            Slot& slot = _slots[_tail & _MASK];
            if (slot._expected_ticket.load(std::memory_order_acquire) != _tail)
                return false;

            // Step 2
            ::new (slot.to_ptr()) T(std::forward<U>(data));

            // Step 3
            slot._expected_ticket.store(_tail + 1, std::memory_order_release);

            // Step 4
            ++_tail;

            // Step 5
            return true;
        }

        // Non-blocking dequeue: Returns nullopt if EMPTY at reservation time.
        //
        // The difference with the blocking pop is that
        // the non-blocking version does not request the slot ownership
        // until its proven that the slot satisfies the pop condition
        // and the operation can be performed (i.e. the trial is going to succeed).
        // This design is mandatory for the non-blocking operations
        // as the non-blocking operations contracts not to modify the state of the queue
        // in case of failure (i.e. the trial has failed).
        //
        // Operation steps: Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
        //   1. Load the _head to the consumer ticket:
        //      std::size_t consumer_ticket = _head.load(std::memory_order_acquire);
        //   2. Inspect if the slot is EMPTY for this consumer ticket:
        //      if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1) return false;
        //   3. Weak CAS the _head to get the ownership of the slot:
        //      if (!_head.compare_exchange_strong(consumer_ticket, consumer_ticket + 1,...)) continue;
        //   4. The slot is owned now, pop the data:
        //      T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
        //   5. If not trivially destructible, call the T's destructor:
        //      if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
        //   6. Mark the slot as EMPTY (expected_ticket = _tail)
        //      Notice that, the EMPTY condition will be satisfied
        //      when the producer reaches the next round of this consumer ticket:
        //      slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
        //   7. return data;
        //
        // Notes:
        //   1. Empty queue inspection of MPMC is an optimization
        //      rather than being a mandatory operation.
        //      This configuration cancels that operation.
        //      See the documentation of try_pop in Concurrent_Queue_LF_Ring_MPMC.hpp for the details.
        //   2. The ABA problem is solved by the monotonous _head ticket.
        //      See the definitions of FULL and EMPTY
        //      given with the definition of _head and _tail members.
        std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            while (true) {
                // Step 1
                std::size_t consumer_ticket = _head.load(std::memory_order_acquire);

                // Step 2
                Slot& slot = _slots[consumer_ticket & _MASK];
                if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1)
                    return std::nullopt;

                // Step 3
                if (
                    !_head.compare_exchange_weak(
                        consumer_ticket,
                        consumer_ticket + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) continue;

                // Step 4
                T* ptr = slot.to_ptr();
                std::optional<T> data{std::move(*ptr)};

                // Step 5
                if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();

                // Step 6
                slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);

                // Step 7
                return data;
            }
        }

        bool empty() const noexcept {
            return _head.load(std::memory_order_acquire) == _tail;
        }

        std::size_t capacity() const noexcept { return _CAPACITY; }
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using queue_LF_ring_SPMC = Concurrent_Queue<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::SPMC,
        T,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_QUEUE_LF_RING_SPMC_HPP
