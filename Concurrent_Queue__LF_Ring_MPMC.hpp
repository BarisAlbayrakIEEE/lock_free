// Concurrent_Queue__LF_Ring_MPMC.hpp
// 
// CAUTION:
//   I created this repository as a reference for my job applications.
//   The code given in this repository is:
//     - to introduce my experience with lock-free concurrency, atomic operations and the required C++ utilities,
//     - to present my background with the concurrent data structures,
//     - not to provide a tested production-ready multi-platform library.
//
// Description:
//   The ticket-based ring buffer solution for the lock-free/ring/MPMC queue problem:
//     Synchronizes the two atomic tickets, _head and _tail,
//     in order to synchronize the producers and consumers.
//     The tickets locate the _head and _tail pointer of the queue
//     while effectively managing the states of each slot (FULL or EMPTY).
//     See the definitions of _head and _tail members of the queue for the details.
//  
//   KEEP IN MIND THAT THE REPOSITORY IS BASED ON THE OPEN SOURCE LOCK-FREE LIBRARY:
//     liblfds: https://liblfds.org/
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
//     2. For an EMPTY slot (i.e. data is popped successfuly) the following equality shall hold:
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
//     1. Increment the _tail to obtain the producer ticket:
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
//     1. Increment the _head to obtain the consumer ticket:
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
//   The original library blocks the _head and _tail pointers and the associated threads
//   until the ticket requirement defined by FULL and EMPTY rules is satisfied.
//   In other words, while a threads is blocked waiting for its reserved slot,
//   the other threads are also blocked as the _head and _tail pointers
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
//     liblfds advances the _tail pointer when the two conditions are satisfied:
//       IF producer_ticket == slot._expected_ticket.load()
//       IF _tail.CAS succeeds
//     My push function advances the _tail pointer non-conditionally.
//   
//   liblfds blocks all threads when one stalls
//   due to this conditional pointer advance.
//   The reason behind this conditional pointer advance is
//   to keep the FIFO order TEMPORALLY SAFE.
//   The thread coming first shall write/read first.
//   However, here in this design, the two pointers always advance.
//   Hence, the FIFO order is preserved ONLY LOGICALLY BUT NOT TEMPORARILY.
//   A producer thread (PT1) arriving earlier may be blocked and write later
//   than another producer (PT2) arriving later.
//   Correspondingly, the data of PT2 may be read before that of PT1.
//   This is SAME AS moodycamel::ConcurrentQueue
//     https://github.com/cameron314/concurrentqueue.git
//
//   liblfds follows the basic invariant of the queue data structure
//   loosing the lock-freedom.
//   However, advancing the _head or _tail pointers unconditionally
//   provides lock-freedom but does not preserve the FIFO order.
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
//
// TODOs:
//   1. The blocking operations (push and pop) back-pressures
//      waiting the expected ticket of the reserved slot to satisfy ticket invariants.
//      An exponential backoff strategy is required for these blocking operations.
//   2. Similar to the 1st one, the edge cases (empty queue and full queue)
//      requires an exponential backoff strategy as well.

#ifndef CONCURRENT_QUEUE_LF_RING_MPMC_HPP
#define CONCURRENT_QUEUE_LF_RING_MPMC_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include "IConcurrent_Queue.hpp"
#include "Concurrent_Queue.hpp"
#include "aux_type_traits.hpp"
#include "cache_line_wrapper.hpp"

namespace BA_Concurrency {
    // use queue_LF_ring_MPMC alias at the end of this file
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
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
        : public IConcurrent_Queue<T>
    {
        using _CLWA = cache_line_wrapper<std::atomic<std::size_t>>;
        static constexpr std::size_t _CAPACITY = pow2_size<Capacity_As_Pow2>;
        static constexpr std::size_t _MASK     = _CAPACITY - 1;

        // Stores the data (T) in a raw byte array instead of storing a T object
        // and performs the construction and destruction manually
        // in push and pop functions respectively.
        // See the documentation of _head and _tail tickets below
        // for _expected_ticket member.
        //
        // aligned to prevent false sharing
        struct alignas(64) Slot {
            std::atomic<std::size_t> _expected_ticket;
            alignas(T) unsigned char _data[sizeof(T)];
            T* to_ptr() noexcept { return std::launder(reinterpret_cast<T*>(_data)); }
        };

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
                const std::size_t consumer_ticket = _head.value.load(std::memory_order_relaxed);
                const std::size_t producer_ticket = _tail.value.load(std::memory_order_relaxed);
                for (std::size_t ticket = consumer_ticket; ticket < producer_ticket; ++p) {
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
        //   1. Increment the _tail to obtain the producer ticket:
        //      const std::size_t producer_ticket = _tail.value.fetch_add(1, std::memory_order_acq_rel);
        //   2. Wait until the slot expects the obtained producer ticket:
        //      while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);
        //   3. The slot is owned now. push the data:
        //      ::new (slot.to_ptr()) T(std::forward<U>(data));
        //   4. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
        //      Notice that, the FULL condition will be satisfied
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
        //      given with the definition of _head and _tail members.
        void push(T data) noexcept(std::is_nothrow_constructible_v<T>) {
            // Step 1
            const std::size_t producer_ticket = _tail.value.fetch_add(1, std::memory_order_acq_rel);
            Slot& slot = _slots[producer_ticket & _MASK];

            // Step 2
            while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);

            // Step 3
            ::new (slot.to_ptr()) T(std::move(data));

            // Step 4
            slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);

            // increment the size
            ++_size;
        }

        // Blocking dequeue: busy-wait while EMPTY at reservation time.
        //
        // Operation steps:
        //   1. Increment the _head to obtain the consumer ticket:
        //      std::size_t consumer_ticket = _head.value.fetch_add(1, std::memory_order_acq_rel);
        //   2. Wait until the slot expects the obtained consumer ticket:
        //      while (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1);
        //   3. The slot is owned now. pop the data:
        //      T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
        //   4. If not trivially destructible, call the T's destructor:
        //      if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
        //   5. Mark the slot as EMPTY (expected_ticket = producer_ticket)
        //      Notice that, the EMPTY condition will be satisfied
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
        //      given with the definition of _head and _tail members.
        std::optional<T> pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            // Step 1
            std::size_t consumer_ticket = _head.value.fetch_add(1, std::memory_order_acq_rel);
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

            // decrement the size
            --_size;

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
        // Operation steps: Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
        //   1. Load the _tail to the producer ticket:
        //      std::size_t producer_ticket = _tail.value.load(std::memory_order_acquire);
        //   2. Inspect if the slot is FULL for this producer ticket:
        //      if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket) return false;
        //   3. Weak CAS the _tail to get the ownership of the slot:
        //      if (!_tail.value.compare_exchange_strong(producer_ticket, producer_ticket + 1,...)) continue;
        //   4. The slot is owned now, push the data:
        //      ::new (slot.to_ptr()) T(std::forward<U>(data));
        //   5. Publish the data by marking it as FULL (expected_ticket = consumer_ticket + 1)
        //      Notice that, the FULL condition will be satisfied
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
        //      given with the definition of _head and _tail members.
        template <class U>
        bool try_push(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            // Step 1
            const std::size_t producer_ticket = _tail.value.load(std::memory_order_acquire);

            // the infinite loop
            while (true) {
                // Step 2
                Slot& slot = _slots[producer_ticket & _MASK];
                if (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket)
                    return false;

                // Step 3
                if (
                    !_tail.value.compare_exchange_weak(
                        producer_ticket,
                        producer_ticket + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) continue;

                // Step 4
                ::new (slot.to_ptr()) T(std::forward<U>(data));

                // Step 5
                slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);

                // increment the size
                ++_size;

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
        // Operation steps: Having an infinite loop at the top to eliminate spurious failure of the weak CAS:
        //   1. Load the _head to the consumer ticket:
        //      std::size_t consumer_ticket = _head.value.load(std::memory_order_acquire);
        //   2. Inspect if the queue is empty:
        //      if (consumer_ticket == _tail.value.load(std::memory_order_acquire)) return std::nullopt;
        //   3. Inspect if the slot is EMPTY for this consumer ticket:
        //      if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1) return false;
        //   4. Weak CAS the _head to get the ownership of the slot:
        //      if (!_head.value.compare_exchange_strong(consumer_ticket, consumer_ticket + 1,...)) continue;
        //   5. The slot is owned now, pop the data:
        //      T* ptr = slot.to_ptr(); std::optional<T> data{ std::move(*ptr) };
        //   6. If not trivially destructible, call the T's destructor:
        //      if constexpr (!std::is_trivially_destructible_v<T>) ptr->~T();
        //   7. Mark the slot as EMPTY (expected_ticket = producer_ticket)
        //      Notice that, the EMPTY condition will be satisfied
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
        //      given with the definition of _head and _tail members.
        std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            // Step 1
            std::size_t consumer_ticket = _head.value.load(std::memory_order_acquire);

            // the infinite loop
            while (true) {
                // Step 2
                if (consumer_ticket == _tail.value.load(std::memory_order_acquire))
                    return std::nullopt;

                // Step 3
                Slot& slot = _slots[consumer_ticket & _MASK];
                if (slot._expected_ticket.load(std::memory_order_acquire) != consumer_ticket + 1)
                    return std::nullopt;

                // Step 4
                if (
                    !_head.value.compare_exchange_weak(
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

                // decrement the size
                --_size;

                // Step 8
                return data;
            }
        }

        inline size_t size() const noexcept override {
            return _size.load();
        }

        inline bool empty() const noexcept override {
            return _size.load() == 0;
        }

        inline std::size_t capacity() const noexcept { return _CAPACITY; }

    private:

        // MEMBERS:
        // The monotonic (only incrementation is allowed) tickets: _head and _tail.
        // The tickets simulates the _head and _tail pointers of the queue data structure.
        // Here, additionally, they semantically act like a state flag
        // which infers if a slot is FULL or EMPTY.
        // Hence, this is a stateful queue design
        // which requires the state invariants to be hold at any time.
        // The tickets exceeds the capacity of the buffer
        // as they increments monotonically.
        // Hence, to achieve the index of a slot, a modulo operation is required.
        // _MASK constant allows performing the modulo operation efficiently
        // using a bitwise mask operation.
        // The state invariants are as follows:
        //   1. For a FULL slot (i.e. contains published data) the following equality shall hold:
        //      slot._expected_ticket == producer_ticket
        //   2. For an EMPTY slot (i.e. does not contain data) the following equality shall hold:
        //      slot._expected_ticket == consumer_ticket + 1
        //
        // This is a flexible state management strategy.
        // For example, for the FIFO to be achieved,
        // a popped slot shall be ready for pushing
        // only when the next round of the slot is reached.
        // We can achieve this condition easily by
        // setting the expected ticket of the slot to the ticket of the next round:
        //   slot._expected_ticket = consumer_ticket + _CAPACITY
        _CLWA _head{0}; // next ticket to pop
        _CLWA _tail{0}; // next ticket to push
        Slot _slots[_CAPACITY];
        std::atomic<size_t> _size{0};
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using queue_LF_ring_MPMC = Concurrent_Queue<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_QUEUE_LF_RING_MPMC_HPP
