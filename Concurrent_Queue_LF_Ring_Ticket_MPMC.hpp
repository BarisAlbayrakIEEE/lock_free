// Concurrent_Queue_LF_Ring_Ticket_MPMC.hpp
//
// Description:
//   The ticket-based solution for the lock-free/ring/MPMC queue problem:
//     Synchronizes the two atomic tickets: _head and _tail
//     in order to synchronize the producers and consumers.
//     The tickets locate the head and tail pointer of the queue
//     while effectively managing the states of each slot (FULL or EMPTY).
//     See the definitions of _head and _tail members of the queue for the details.
//
// Requirements:
// - _CAPACITY must be a power of two (for fast masking).
// - T must be noexcept-constructible.
// - T must be noexcept-movable.
//
// Invariant:
//   See the definitions of _head and _tail for the FULL and EMPTY states.
//   Producers and consumers hold the state invariant.
//
// Semantics:
//   Slot class:
//     The ring buffer is a contiguous array of Slot objects.
//     A slot encapsulattes two members:
//       1. The data is stored in a byte array.
//       2. The expected ticket of the slot
//          which flexibly defines the state of the slot as FULL or EMPTY.
//
//   The threads are completely isolated by the well aligned slots (no false sharing)
//   such that each thread works on a different slot at any time.
//   This is provided by the ticket approach.
//
//   The producers rely on the shared _tail ticket
//   while the consumers use the _head ticket.
//   Hence, the only contention is on the _head and _tail tickets
//   among the producers and consumers respectively.
//   The producer threads serialize at _tail while
//   the consumers serialize at _head.
//   The only exception is try_pop method
//   which loads _tail for an optimization for the empty-queue edge case.
//   See the Cautions section below which states that
//   the single consumer configurations exclude the use of _tail ticket
//   in order to optimize the _tail synchronization.
//   
//   The shared use of the _head and _tail tickets
//   disappears for the single producer and single consumer configurations
//   keeping in mind that these configurations
//   will exclude _tail in try_pop function.
//   The single producer configurations will replace
//   the atomic type of the _tail ticket with a regular non-atomic type,
//   while the single consumer configurations will do the same for the _head ticket.
//   There exist other issues for the optimization which are discussed
//   in the documentation of the corresponding header file.
//   
//
//   push():
//     1. Fetch increment the tail to obtain the producer ticket:
//        const std::size_t producer_ticket = _tail.fetch_add(1, std::memory_order_acq_rel);
//     2. Wait until the slot expects the obtained producer ticket:
//        while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);
//     3. The slot is owned now. push the data:
//        ::new (slot.to_ptr()) T(std::forward<U>(data));
//     4. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
//        Notice that, the FULL condition will happen
//        when the consumer ticket reaches the current producer ticket:
//        slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);
//
//   pop():
//     1. Fetch increment the head to obtain the consumer ticket:
//        std::size_t consumer_ticket = _head.fetch_add(1, std::memory_order_acq_rel);
//     2. Wait until the slot expects the obtained consumer ticket:
//        while (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1);
//     3. The slot is owned now. pop the data:
//        T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
//     4. If not trivially destructible, call the T's destructor:
//        if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
//     5. Mark the slot as EMPTY (expected_ticket = producer_ticket)
//        Notice that, the EMPTY condition will happen
//        when the producer reaches the next round of this consumer ticket:
//        slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
//     6. return data;
//
//   try_push():
//     1. Load the _tail to the producer ticket:
//        std::size_t producer_ticket = _tail.load(std::memory_order_acquire);
//     2. Inspect if the slot is FULL for this producer ticket:
//        if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket) return false;
//     3. CAS the _tail to get the ownership of the slot:
//        if (!_tail.compare_exchange_strong(producer_ticket, producer_ticket + 1,...)) continue;
//     4. The slot is owned now, push the data:
//        ::new (slot.to_ptr()) T(std::forward<U>(data));
//     5. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
//        Notice that, the FULL condition will happen
//        when the consumer ticket reaches the current producer ticket:
//        slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);
//     6. return true;
//
//   try_pop():
//     1. Load the _head to the consumer ticket:
//        std::size_t consumer_ticket = _head.load(std::memory_order_acquire);
//     2. Inspect if the queue is empty (an optimization for the empty case):
//        if (consumer_ticket == _tail.load(std::memory_order_acquire)) return std::nullopt;
//     3. Inspect if the slot is EMPTY for this producer ticket:
//        if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1) return false;
//     4. CAS the _head to get the ownership of the slot:
//        if (!_head.compare_exchange_strong(consumer_ticket, consumer_ticket + 1,...)) continue;
//     5. The slot is owned now, pop the data:
//        T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
//     6. If not trivially destructible, call the T's destructor:
//        if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
//     7. Mark the slot as EMPTY (expected_ticket = producer_ticket)
//        Notice that, the EMPTY condition will happen
//        when the producer reaches the next round of this consumer ticket:
//        slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
//     8. return data;
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
//     queue_LF_ring_ticket_MPSC = queue_LF_ring_ticket_MPMC
//     queue_LF_ring_ticket_SPMC = queue_LF_ring_ticket_MPMC
//     queue_LF_ring_ticket_SPSC = queue_LF_ring_ticket_MPMC
//   See the end of this header for the alias definitiions.
//
// CAUTION:
//   use queue_LF_ring_ticket_MPMC alias at the end of this file
//   to get the right specialization of Concurrent_Queue
//   and to achieve the default arguments consistently.
//
// CAUTION:
//   See Concurrent_Queue_LF_Ring_Ticket_MPMC for ticket-based version
//   which guarantees lock-free execution.

#ifndef CONCURRENT_QUEUE_LF_RING_TICKET_MPMC_HPP
#define CONCURRENT_QUEUE_LF_RING_TICKET_MPMC_HPP

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
    // use queue_LF_ring_ticket_MPMC alias at the end of this file
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
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Ticket)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
    {
        static constexpr std::size_t _CAPACITY = pow2_size<Capacity_As_Pow2>;
        static constexpr std::size_t _MASK     = _CAPACITY - 1;

        // Stores the data (T) in a raw byte array instead of storing a T object
        // and performs the construction and destruction manually
        // in push and pop functions respectively.
        struct alignas(64) Slot {
            std::atomic<std::size_t> _expected_ticket;
            alignas(T) unsigned char _data[sizeof(T)];
            T* to_ptr() noexcept { return std::launder(reinterpret_cast<T*>(_data)); }
        };

        // The _head and _tail tickets semantically mark the state of a slot as FULL or EMPTY
        // For a FULL slot (i.e. contains published data) the following equality holds:
        //   slot._expected_ticket == producer_ticket
        // For an EMPTY slot (i.e. does not contain data) the following equality holds:
        //   slot._expected_ticket == consumer_ticket + 1
        //
        // This is a flexible state management strategy.
        // For example, the ticket-based ring queue requires
        // that a popped slot shall be ready for pushing
        // only when the next round of the slot is reached.
        // We can achieve this condition easily by
        // setting the expected ticket of the slot to the ticket of the next round:
        //   slot._expected_ticket = consumer_ticket + _CAPACITY
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
        //   1. This producer function does not share data with the consumers
        //      which is a significant detail for the optimization of
        //      the single producer configurations (SPMC and SPSC).
        //   2. Back-pressures when the queue is full by spinning on its reserved slot.
        //   3. If stalls, only its reserved slot delays
        //      but does not block others from operating on the other slots.
        //   4. The ABA problem is solved by the monotonous _tail ticket.
        //      See the definitions of FULL and EMPTY
        //      given with the decleration of _head  and _tail tickets.
        template <class U>
        void push(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
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
        //      when the producer reaches the next round of this consumer ticket:
        //      slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
        //   6. return data;
        //
        // Notes:
        //   1. This consumer function does not share data with the producers
        //      which is a significant detail for the optimization of
        //      the single consumer configurations (MPSC and SPSC).
        //   2. Back-pressures when the queue is empty by spinning on its reserved slot.
        //   3. If stalls, only its reserved slot delays
        //      but does not block others from operating on the other slots.
        //   4. The ABA problem is solved by the monotonous _head ticket.
        //      See the definitions of FULL and EMPTY
        //      given with the decleration of _head  and _tail tickets.
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
        // Operation steps:
        //   1. Load the _tail to the producer ticket:
        //      std::size_t producer_ticket = _tail.load(std::memory_order_acquire);
        //   2. Inspect if the slot is FULL for this producer ticket:
        //      if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket) return false;
        //   3. CAS the _tail to get the ownership of the slot:
        //      if (!_tail.compare_exchange_strong(producer_ticket, producer_ticket + 1,...)) continue;
        //   4. The slot is owned now, push the data:
        //      ::new (slot.to_ptr()) T(std::forward<U>(data));
        //   5. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
        //      Notice that, the FULL condition will happen
        //      when the consumer ticket reaches the current producer ticket:
        //      slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);
        //   6. return true;
        //
        // Notes:
        //   1. This producer function does not share data with the consumers
        //      which is a significant detail for the optimization of
        //      the single producer configurations (SPMC and SPSC).
        //   2. The ABA problem is solved by the monotonous _tail ticket.
        //      See the definitions of FULL and EMPTY
        //      given with the decleration of _head  and _tail tickets.
        template <class U>
        bool try_push(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            // Step 1
            const std::size_t producer_ticket = _tail.load(std::memory_order_acquire);
            while (true) {
                Slot& slot = _slots[producer_ticket & _MASK];

                // Step 2
                if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket)
                    return false;

                // Step 3
                if (
                    !_tail.compare_exchange_strong(
                        producer_ticket,
                        producer_ticket + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) continue;

                // Step 4
                ::new (slot.to_ptr()) T(std::forward<U>(data));

                // Step 5
                slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);

                // Step 6
                return true;
            }
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
        // Operation steps:
        //   1. Load the _head to the consumer ticket:
        //      std::size_t consumer_ticket = _head.load(std::memory_order_acquire);
        //   2. Inspect if the queue is empty:
        //      if (consumer_ticket == _tail.load(std::memory_order_acquire)) return std::nullopt;
        //   3. Inspect if the slot is EMPTY for this producer ticket:
        //      if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1) return false;
        //   4. CAS the _head to get the ownership of the slot:
        //      if (!_head.compare_exchange_strong(consumer_ticket, consumer_ticket + 1,...)) continue;
        //   5. The slot is owned now, pop the data:
        //      T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
        //   6. If not trivially destructible, call the T's destructor:
        //      if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
        //   7. Mark the slot as EMPTY (expected_ticket = producer_ticket)
        //      Notice that, the EMPTY condition will happen
        //      when the producer reaches the next round of this consumer ticket:
        //      slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
        //   8. return data;
        //
        // Notes:
        //   1. Step 2 yields a data share between the producer and consumer threads: _tail.
        //      Here, _tail is used to detect the empty queue situation
        //      earlier with a cheap operation (i.e. atomic load)
        //      before using CAS operations which requires much more CPU resurces.
        //      Its not mandatory but an optimization.
        //      SPMC and SPSC configurations would exclude the _tail use in try_pop method
        //      in order to separate the producers and consumers completely
        //      and to optimize the two tickets: _head and _tail.
        //      This will cause a little performance loss in case of empty queues
        //      for SPMC and SPSC but let the optimization be more effective for the other functions.
        //      For example, the SPSC configuration would not require
        //      atomic types for _head and _tail anymore.
        //   2. The ABA problem is solved by the monotonous _head ticket.
        //      See the definitions of FULL and EMPTY
        //      given with the decleration of _head  and _tail tickets.
        std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            // Step 1
            std::size_t consumer_ticket = _head.load(std::memory_order_acquire);

            while (true) {
                // Step 2
                if (consumer_ticket == _tail.load(std::memory_order_acquire))
                    return std::nullopt;

                // Step 3
                Slot& slot = _slots[consumer_ticket & _MASK];
                if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1)
                    return std::nullopt;

                // Step 4
                if (
                    !_head.compare_exchange_strong(
                        consumer_ticket,
                        consumer_ticket + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) continue;

                // Step 5
                T* ptr = slot.to_ptr();
                std::optional<T> data{std::move(*ptr)};

                // Step 6
                if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();

                // Step 7
                slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);

                // Step 8
                return data;
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
    using queue_LF_ring_ticket_MPMC = Concurrent_Queue<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Ticket)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_QUEUE_LF_RING_TICKET_MPMC_HPP
