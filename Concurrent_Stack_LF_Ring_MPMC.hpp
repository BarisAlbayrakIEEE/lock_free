// Stack: bounded lock-free MPMC stack over a contiguous ring buffer.
// Requirements:
// - capacity must be a power of two (for fast masking).
// - T must be MoveConstructible, and ideally noexcept-movable for best guarantees.
//
// Semantics:
// - push():
//     reserves a unique "ticket" via fetch_add on _top (monotonic).
//     The slot for that ticket is (ticket & mask). Producer waits until the
//     slot's buffer index equals ticket (meaning it's empty for this cycle),
//     constructs T in-place, then publishes by setting buffer index = ticket + 1.
// - pop():
//     grabs the most-recent ticket by CAS-decrementing _top. That yields
//     ticket = old_top - 1 (LIFO). Consumer waits until the slot's buffer index equals
//     ticket + 1 (meaning full), moves the data, destroys it, then marks the
//     slot empty for the *next* wraparound by setting buffer index = ticket + capacity.
//
// Progress:
// - Lock-free:
//     A stalled thread can delay a specific slot but does
//     not block others from operating on other slots.
// - push():
//     back-pressures when the stack is full by spinning on its reserved slot.
// - pop():
//     returns empty immediately if it cannot reserve a ticket (_top == 0).
//
// Notes:
// - No dynamic allocation or reclamation:
//     ABA is avoided by per-slot buffer index.
// - Memory orders chosen to release data before visibility of "full" and to acquire data after observing "full".

#ifndef CONCURRENT_STACK_LF_RING_MPMC_HPP
#define CONCURRENT_STACK_LF_RING_MPMC_HPP

#include <cstddef>
#include <atomic>
#include <new>
#include <type_traits>
#include <optional>
#include "Concurrent_Stack.hpp"

namespace BA_Concurrency {
    template <unsigned char power>
    struct pow2_size_t {
        static constexpr std::size_t value = std::size_t(1) << power;
    };
    
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
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
    {
        struct alignas(64) Slot {
            // _state:
            //     0: producer/continue
            //     1: producer/done
            //     2: consumer/continue
            //     3: consumer/done
            std::atomic<std::uint64_t> _state; // 0: producer (push); 1: consumer (pop)
            std::atomic<std::uint64_t> _state_progress; // 0: continue; 1: done
            alignas(64) T _data;
            Slot() noexcept : _state(3) {};
        };

        // mask to modulo the ticket by capacity
        static constexpr std::size_t capacity = std::size_t(1) << Capacity_As_Pow2;
        static constexpr std::size_t _MASK = capacity - 1;

        // _top is the next ticket to push.
        alignas(64) std::atomic<std::uint64_t> _top{0};
        // the buffer of slots
        Slot _slots[capacity];

    public:

        // Non-copyable / non-movable for simplicity
        Stack(const Stack&) = delete;
        Stack& operator=(const Stack&) = delete;

        void push(const T& data) noexcept {
            // Try to claim a slot with consumer/done state
            std::uint64_t top = _top.load(std::memory_order_relaxed) % capacity;
            Slot& slot = _slots[top];
            uint8_t expected_state = 3; // consumer/done state
            while (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    0, // producer/progress state
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
                top = _top.fetch_add(1, std::memory_order_release) % capacity;
                slot = _slots[top];
            };

            // mark the progress state as done and write the data
            slot._state.store(1, std::memory_order_release);
            slot._data = data;
        }

        T pop() noexcept {
            // Try to claim a slot with producer/done state
            std::uint64_t top = _top.load(std::memory_order_relaxed) % capacity;
            Slot& slot = _slots[top];
            uint8_t expected_state = 1; // producer/done state
            while (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    2, // consumer/progress state
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
                top = _top.fetch_sub(1, std::memory_order_release) % capacity;
                slot = _slots[top];
            };

            // mark the progress state as done and return data
            slot._state.store(3, std::memory_order_release);
            return slot._data;
        }

        bool try_push(const T& data) noexcept {
            // Try to claim a slot with consumer/done state
            std::uint64_t top = _top.load(std::memory_order_relaxed) % capacity;
            Slot& slot = _slots[top];
            uint8_t expected_state = 3; // consumer/done state
            if (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    0, // producer/progress state
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
                return false;
            };

            // mark the progress state as done and write the data
            slot._state.store(1, std::memory_order_release);
            slot._data = data;
            return true;
        }

        std::optional<T> try_pop() noexcept {
            // Try to claim a slot with producer/done state
            std::uint64_t top = _top.load(std::memory_order_relaxed) % capacity;
            Slot& slot = _slots[top];
            uint8_t expected_state = 1; // producer/done state
            if (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    2, // consumer/progress state
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
                return std::nullopt;
            };

            // mark the progress state as done and return data
            slot._state.store(3, std::memory_order_release);
            return slot._data;
        }
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using stack_LF_ring_MPMC = Concurrent_Stack<
        true,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_LF_RING_MPMC_HPP
