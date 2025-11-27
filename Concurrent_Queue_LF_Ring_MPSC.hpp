// Concurrent_Queue_LF_Ring_MPSC.hpp
// 
// CAUTION:
//   I created this repository as a reference for my job applications.
//   The code given in this repository is:
//     - to introduce my experience with lock-free concurrency, atomic operations and the required C++ utilities,
//     - to present my background with the concurrent data structures,
//     - not to provide a tested production-ready multi-platform library.
//
// CAUTION:
//   Currently contains an initial version based on the discussions
//   held in Concurrent_Queue_LF_Ring_MPMC.hpp.

#ifndef CONCURRENT_QUEUE_LF_RING_MPSC_HPP
#define CONCURRENT_QUEUE_LF_RING_MPSC_HPP

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
    // use queue_LF_ring_MPSC alias at the end of this file
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
        Enum_Concurrency_Models::MPSC,
        T,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
        : public IConcurrent_Queue<T>
    {
        using _CLWN = cache_line_wrapper<std::size_t>;
        using _CLWA = cache_line_wrapper<std::atomic<std::size_t>>;
        static constexpr std::size_t _CAPACITY = pow2_size<Capacity_As_Pow2>;
        static constexpr std::size_t _MASK     = _CAPACITY - 1;

        // See Concurrent_Queue_LF_Ring_MPMC.hpp for the descriptions
        // Notice that currently the only difference with Concurrent_Queue_LF_Ring_MPMC.hpp
        // is the non-atomic head ticket.
        struct alignas(64) Slot {
            std::atomic<std::size_t> _expected_ticket;
            alignas(T) unsigned char _data[sizeof(T)];
            T* to_ptr() noexcept { return std::launder(reinterpret_cast<T*>(_data)); }
        };

    public:

        // See Concurrent_Queue_LF_Ring_MPMC.hpp for the descriptions
        // Notice that currently the only difference with Concurrent_Queue_LF_Ring_MPMC.hpp
        // is the non-atomic head ticket.
        Concurrent_Queue() noexcept {
            for (std::size_t i = 0; i < _CAPACITY; ++i) {
                _slots[i]._expected_ticket.store(i, std::memory_order_relaxed); // exoected = producer ticket
            }
        }

        // See Concurrent_Queue_LF_Ring_MPMC.hpp for the descriptions
        // Notice that currently the only difference with Concurrent_Queue_LF_Ring_MPMC.hpp
        // is the non-atomic head ticket.
        ~Concurrent_Queue() {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                const std::size_t consumer_ticket = _head.value;
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

        // see the documentation of push_helper
        inline void push(const T& data) override {
            push_helper(data);
        }
        inline void push(T&& data) override {
            push_helper(std::move(data));
        }

        // See Concurrent_Queue_LF_Ring_MPMC.hpp for the descriptions
        // Notice that currently the only difference with Concurrent_Queue_LF_Ring_MPMC.hpp
        // is the non-atomic head ticket.
        std::optional<T> pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            // Step 1
            std::size_t consumer_ticket = _head.value++;
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

        // See Concurrent_Queue_LF_Ring_MPMC.hpp for the descriptions
        // Notice that currently the only difference with Concurrent_Queue_LF_Ring_MPMC.hpp
        // is the non-atomic head ticket.
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

        // See Concurrent_Queue_LF_Ring_MPMC.hpp for the descriptions
        // Notice that currently the only difference with Concurrent_Queue_LF_Ring_MPMC.hpp
        // is the non-atomic head ticket.
        std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            // Step 1
            std::size_t consumer_ticket = _head.value

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
                ++_head.value;

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

        size_t size() const override noexcept {
            return _size.load();
        }

        bool empty() const noexcept {
            return
                _head.value ==
                _tail.value.load(std::memory_order_acquire);
        }

        std::size_t capacity() const noexcept { return _CAPACITY; }

    private:

        // See Concurrent_Queue_LF_Ring_MPMC.hpp for the descriptions
        // Notice that currently the only difference with Concurrent_Queue_LF_Ring_MPMC.hpp
        // is the non-atomic head ticket.
        template <class U>
        void push_helper(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            // Step 1
            const std::size_t producer_ticket = _tail.value.fetch_add(1, std::memory_order_acq_rel);
            Slot& slot = _slots[producer_ticket & _MASK];

            // Step 2
            while (slot._expected_ticket.load(std::memory_order_acquire) != producer_ticket);

            // Step 3
            ::new (slot.to_ptr()) T(std::forward<U>(data));

            // Step 4
            slot._expected_ticket.store(producer_ticket + 1, std::memory_order_release);

            // increment the size
            ++_size;
        }

        // See Concurrent_Queue_LF_Ring_MPMC.hpp for the descriptions
        // Notice that currently the only difference with Concurrent_Queue_LF_Ring_MPMC.hpp
        // is the non-atomic head ticket.
        _CLWN _head{0}; // next ticket to pop
        _CLWA _tail{0}; // next ticket to push
        Slot _slots[_CAPACITY];
        std::atomic<size_t> _size{0};
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using queue_LF_ring_MPSC = Concurrent_Queue<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPSC,
        T,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_QUEUE_LF_RING_MPSC_HPP
