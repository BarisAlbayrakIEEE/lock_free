// Concurrent_Queue_LF_Ring_MPMC.hpp
//
// Description:
//   The ticket-based solution for the lock-free/ring/MPMC queue problem:
//     Synchronizes the two atomic tickets: _head and _tail
//     in order to synchronize the producers and consumers.
//     The tickets locate the head and tail pointer of the queue
//     while effectively managing the states of each slot (FULL or EMPTY).
//     See the definitions of _head and _tail members of the queue for the details.
//  
//   KEEP IN MIND THAT THE REPOSITORY IS BASED ON THE OPEN SOURCE LOCK-FREE LIBRARY:
//     liiblfds: https://liblfds.org/
//
// Requirements:
// - T must be noexcept-constructible.
// - T must be noexcept-movable.
//
// Invariants:
//   Producers and consumers shall hold the state invariant.
//   See the definitions of _head and _tail members for the tickets
//   that allow managing the slot states.
//   The state invariants are as follows:
//     1. For a FULL slot (i.e. contains published data) the following equality shall hold:
//        slot._expected_ticket == producer_ticket
//     2. For an EMPTY slot (i.e. does not contain data) the following equality shall hold:
//        slot._expected_ticket == consumer_ticket + 1
//
// Semantics:
//   Slot class:
//     The ring buffer is a contiguous array of Slot objects.
//     A slot encapsulates two members:
//       1. The data is stored in a byte array of size of T.
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
//   push():
//     1. Increment the tail to obtain the producer ticket:
//        const std::size_t producer_ticket = _tail.value.fetch_add(1, std::memory_order_acq_rel);
//     2. Wait until the slot expects the obtained producer ticket:
//        while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);
//     3. The slot is owned now. push the data:
//        ::new (slot.to_ptr()) T(std::forward<U>(data));
//     4. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
//        Notice that, the FULL condition will be satisfied
//        when the consumer ticket reaches the current producer ticket:
//        slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);
//
//   pop():
//     1. Increment the head to obtain the consumer ticket:
//        std::size_t consumer_ticket = _head.value.fetch_add(1, std::memory_order_acq_rel);
//     2. Wait until the slot expects the obtained consumer ticket:
//        while (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1);
//     3. The slot is owned now. pop the data:
//        T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
//     4. If not trivially destructible, call the T's destructor:
//        if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
//     5. Mark the slot as EMPTY (expected_ticket = producer_ticket)
//        Notice that, the EMPTY condition will be satisfied
//        when the producer reaches the next round of this consumer ticket:
//        slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
//     6. return data;
//
//   try_push(): Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
//     1. Load the _tail to the producer ticket:
//        std::size_t producer_ticket = _tail.value.load(std::memory_order_acquire);
//     2. Inspect if the slot is FULL for this producer ticket:
//        if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket) return false;
//     3. Weak CAS the _tail to get the ownership of the slot:
//        if (!_tail.value.compare_exchange_strong(producer_ticket, producer_ticket + 1,...)) continue;
//     4. The slot is owned now, push the data:
//        ::new (slot.to_ptr()) T(std::forward<U>(data));
//     5. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
//        Notice that, the FULL condition will be satisfied
//        when the consumer ticket reaches the current producer ticket:
//        slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);
//     6. return true;
//
//   try_pop(): Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
//     1. Load the _head to the consumer ticket:
//        std::size_t consumer_ticket = _head.value.load(std::memory_order_acquire);
//     2. Inspect if the queue is empty (an optimization for the empty case):
//        if (consumer_ticket == _tail.value.load(std::memory_order_acquire)) return std::nullopt;
//     3. Inspect if the slot is EMPTY for this consumer ticket:
//        if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1) return false;
//     4. Weak CAS the _head to get the ownership of the slot:
//        if (!_head.value.compare_exchange_strong(consumer_ticket, consumer_ticket + 1,...)) continue;
//     5. The slot is owned now, pop the data:
//        T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
//     6. If not trivially destructible, call the T's destructor:
//        if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
//     7. Mark the slot as EMPTY (expected_ticket = producer_ticket)
//        Notice that, the EMPTY condition will be satisfied
//        when the producer reaches the next round of this consumer ticket:
//        slot._expected_ticket.store(consumer_ticket + _CAPACITY, std::memory_order_release);
//     8. return data;
//
// Progress:
//   The original algorithm (liblfds) is based on Dmitry Vyukov's lock-free queue:
//   and is not lock-free:
//     https://stackoverflow.com/a/54755605
//
//   The original library blocks the head and tail pointers and the associated threads
//   until the ticket requirement defined by FULL and EMPTY rulesis satisfied.
//   In other words, while a threads is blocked waiting for its reserved slot,
//   the other threads are also blocked as the head and tail pointers
//   are only advanced when the thread achieves to satisfy the ticket condition.
//   In summary push function of the original algorithm is as follows:
//     1. producer_ticket = _tail.load()
//     2. INFINITE LOOP
//     3.   IF producer_ticket == slot._expected_ticket.load())
//     4.     IF _tail.CAS(producer_ticket, producer_ticket + 1)
//     5.       BREAK
//     6. Push the data
//     7. slot._expected_ticket.store(producer_ticket + 1);
//     
//   My push function:
//     1. producer_ticket = _tail.fetch_add()
//     2. while(!slot._expected_ticket.load() != producer_ticket);
//     3. Push the data
//     4. slot._expected_ticket.store(producer_ticket + 1);
//   
//   The difference between the two algorithms is:
//     liblfds advances the tail pointer when the two conditions are satisfied:
//       IF producer_ticket == slot._expected_ticket.load()
//       IF _tail.CAS succeeds
//     My push function advances the tail pointer non-conditionally.
//   
//   liblfds blocks all threads when one stalls
//   due to this conditional pointer advance.
//   The reason behind this conditional pointer advance is
//   to keep the FIFO order TEMPORALLY SAFE.
//   The thread coming first shall right first.
//   However, here in this design, the two pointers always advance.
//   Hence, the FIFO order is not preserved temporally.
//   A producer thread (PT1) arriving earlier may be blocked and write later
//   than another producer (PT2) arriving later.
//   Correspondingly, the data of PT2 will be read before that of PT1.
//   This is a fundamental structural failure for a queue data structure!
//
//   liblfds follows the basic invariant of the queue data structure
//   loosing the lock-freedom.
//   However, advancing the head or tail pointers unconditionally
//   provides lock-freedom but does not preserve the FIFO order.
//
//   A real solution to the problem is to manage the threads such that
//   when a thread stalls another one can take over its work and finish it.
//   There are a number of approaches in this respect:
//     1. 
//     2. 
//     3. 
//     4. 
//     5. 
//     6. 
//     7. 
//
//   Concurrent_Queue_LF_Ring_XXX_MPMC.hpp introduces the solution with XXX.
//
// Notes:
//   1. Memory orders are chosen to
//      release data before the visibility of the state transitions and
//      to acquire data after observing the state transitions.
//   2. push(): Back-pressures when the queue is full by spinning on its reserved slot.
//      pop(): Back-pressures when the queue is empty by spinning on its reserved slot.
//   3. The optimizations for single producer/consumer configurations
//      can be found in the following header files:
//        queue_LF_ring_MPSC.hpp
//        queue_LF_ring_SPMC.hpp
//        queue_LF_ring_SPSC.hpp
//
// Cautions:
//   1. Threads may spin indefinitely if a counterpart thread fails mid-operation,
//      before setting the expected state accordingly.
//   2. Use queue_LF_ring_MPMC alias at the end of this file
//      to get the right specialization of Concurrent_Queue
//      and to achieve the default arguments consistently.
//   3. As stated in the Progress section, 
//      this version does not preserve the FIFO order.
//      See Concurrent_Queue_LF_Ring_XXX_MPMC.hpp
//      for a solution based on XXX approach.
//
// TODOs:
//   1. The blocking operations (push and pop) back-pressures
//      waiting the expected ticket of the reserved slot to satisfy ticket invariants.
//      An exponential backoff strategy is required for these blocking operations.
//   2. Similar to the 1st one, the edge cases (empty queue and full queue)
//      requires an exponential backoff strategy as well.
//      Currently, only try_pop takes the edge condition into account.

#ifndef CONCURRENT_QUEUE_LF_RING_MPMC_HPP
#define CONCURRENT_QUEUE_LF_RING_MPMC_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <cassert>
#include <utility>
#include "Concurrent_Queue.hpp"
#include "aux_type_traits.hpp"
#include "cache_line_wrapper.hpp"
#include "order_compliances.hpp"

namespace BA_Concurrency {
    // TODO: XXX
    // Concurrent_Queue_LF_Ring_Stamped_MPMC.hpp
    // 
    // Description:
    //   Lock-free MPMC queue using ring buffer with versioned slots.
    //   Inspired by Vyukov's design, with version counters to protect slot state.
    //
    // Requirements:
    //   - T must be noexcept-movable.
    template <typename T, unsigned char Capacity_As_Pow2>
    requires(std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_assignable_v<T>)
    class Concurrent_Queue_LF_Ring_Stamped_MPMC {
    private:
        enum SlotState : std::uint8_t {
            Empty = 0,
            Full = 1
        };

        struct Slot {
            alignas(64) std::atomic<std::size_t> _version;
            alignas(alignof(T)) std::byte _data[sizeof(T)];

            T* to_ptr() noexcept {
                return std::launder(reinterpret_cast<T*>(&_data));
            }
        };

        static constexpr std::size_t _CAPACITY = pow2_size<Capacity_As_Pow2>;
        static constexpr std::size_t _MASK = _CAPACITY - 1;

        alignas(64) std::atomic<std::size_t> _head{0};
        alignas(64) std::atomic<std::size_t> _tail{0};
        Slot _slots[_CAPACITY];

    public:

        // Initialize each slot to expect its index as the first producer ticket
        Concurrent_Queue_LF_Ring_Stamped_MPMC() {
            for (std::size_t i = 0; i < _CAPACITY; ++i) {
                _slots[i]._version.store(i * 2, std::memory_order_relaxed);
            }
        }

        // Single-threaded context expected.
        // destroy the elements that were enqueued but not yet dequeued
        ~Concurrent_Queue_LF_Ring_Stamped_MPMC() {
            std::size_t consumer_ticket = _head.load(std::memory_order_relaxed);
            std::size_t producer_ticket = _tail.load(std::memory_order_relaxed);
            while (consumer_ticket < producer_ticket) {
                auto& slot = _slots[consumer_ticket & _MASK];
                std::destroy_at(slot.to_ptr());
                ++consumer_ticket;
            }
        }

        // Non-copyable/movable for simplicity
        Concurrent_Queue(const Concurrent_Queue&) = delete;
        Concurrent_Queue& operator=(const Concurrent_Queue&) = delete;
        Concurrent_Queue(Concurrent_Queue&&) = delete;
        Concurrent_Queue& operator=(Concurrent_Queue&&) = delete;

        template <class U>
        bool try_push(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            std::size_t producer_ticket = _tail.load(std::memory_order_relaxed);
            while (true) {
                Slot& slot = _slots[producer_ticket & _MASK];
                std::size_t expected_version = producer_ticket * 2;
                if (slot._version.load(std::memory_order_acquire) != expected_version)
                    return false;

                if (
                    _tail.compare_exchange_weak(
                        producer_ticket,
                        producer_ticket + 1,
                        std::memory_order_acquire, // TODO: XXX: std::memory_order_acq_rel?
                        std::memory_order_relaxed))
                {
                    std::construct_at(slot.to_ptr(), std::forward<U>(data));
                    slot._version.store(expected_version + 1, std::memory_order_release);
                    return true;
                }
            }
        }

        std::optional<T> try_pop() {
            std::size_t consumer_ticket = _head.load(std::memory_order_relaxed);
            while (true) {
                Slot& slot = _slots[consumer_ticket & _MASK];
                std::size_t expected_version = consumer_ticket * 2 + 1;
                if (slot._version.load(std::memory_order_acquire) != expected_version)
                    return std::nullopt;

                if (
                    _head.compare_exchange_weak(
                        consumer_ticket,
                        consumer_ticket + 1,
                        std::memory_order_acquire, // TODO: XXX: std::memory_order_acq_rel?
                        std::memory_order_relaxed))
                {
                    T data = std::move(*slot.to_ptr());
                    std::destroy_at(slot.to_ptr());
                    slot._version.store((consumer_ticket + _CAPACITY) * 2, std::memory_order_release);
                    return data;
                }
            }
        }

        bool empty() const noexcept {
            return
                _head.load(std::memory_order_acquire) ==
                _tail.load(std::memory_order_acquire);
        }
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using queue_LF_ring_MPMC = Concurrent_Queue<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<unsigned char, Capacity_As_Pow2>,
        FIFO_None>;
} // namespace BA_Concurrency

#endif // CONCURRENT_QUEUE_LF_RING_MPMC_HPP
