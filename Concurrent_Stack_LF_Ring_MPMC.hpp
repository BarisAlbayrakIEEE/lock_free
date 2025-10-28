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
            std::atomic<std::uint64_t> _state_owner; // 0: consumer (pop); 1: producer (push)
            std::atomic<std::uint64_t> _state_progress; // 0: continue; 1: done
            alignas(64) T _data;
            Slot() noexcept : owner(0), progress(1) {};
        };

        // mask to modulo the ticket by capacity
        static constexpr std::size_t capacity = std::size_t(1) << Capacity_As_Pow2;
        static constexpr std::size_t _MASK = capacity - 1;

        // _top is the next ticket to push.
        alignas(64) std::atomic<std::uint64_t> _top{0};
        // the buffer of slots
        Slot _slots[capacity];












    bool try_push(const T& v) noexcept {
        std::size_t index = _top.load(std::memory_order_relaxed) % capacity;
        Slot& slot = _slots[index];

        // Step 1: Try to claim slot if it's in CONSUMER/DONE state
        uint8_t expected_owner = 0;  // consumer
        if (
            !slot.owner.compare_exchange_strong(
                expected_owner,
                1, 
                std::memory_order_acquire,
                std::memory_order_relaxed))
            return false; // slot still in use

        uint8_t expected_progress = 1; // done
        if (
            !slot.progress.compare_exchange_strong(
                expected_progress,
                0, 
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            slot.owner.store(0, std::memory_order_release);
            return false;
        }

        // Step 2: write data
        slot.value = data;

        // Step 3: mark as done
        slot.progress.store(1, std::memory_order_release);

        // Step 4: advance top
        _top.fetch_add(1, std::memory_order_release);
        return true;
    }

    std::optional<T> try_pop() noexcept {
        std::size_t old_top = _top.load(std::memory_order_acquire);
        if (old_top == 0) return std::nullopt;  // empty

        std::size_t index = (old_top - 1) % capacity;
        Slot& slot = _slots[index];

        // Step 1: Try to claim slot if it's in PRODUCER/DONE state
        uint8_t expected_owner = 1;
        if (!slot.owner.compare_exchange_strong(expected_owner, 0, 
                std::memory_order_acquire, std::memory_order_relaxed))
            return std::nullopt;  // producer still owns

        uint8_t expected_progress = 1;
        if (!slot.progress.compare_exchange_strong(expected_progress, 0, 
                std::memory_order_acquire, std::memory_order_relaxed)) {
            // revert owner
            slot.owner.store(1, std::memory_order_release);
            return std::nullopt;
        }

        // Step 2: read data
        T out = slot.value;

        // Step 3: mark as done (empty)
        slot.progress.store(1, std::memory_order_release);

        // Step 4: decrement top
        _top.fetch_sub(1, std::memory_order_release);

        return out;
    }        






















        template <class U>
        bool push_nonblocking(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            auto old_top = _top.load(std::memory_order_acquire);
            do {
                do {

                } while (

                )

            } while (
                _top.compare_exchange_strong(
                    old_top,
                    old_top + 1,
                    std::memory_order_acquire,
                    std::memory_order_release))



            auto old_top = _top.load(std::memory_order_acquire);
            auto state_owner = _slots[old_top]._state_owner.load(std::memory_order_acquire);
            auto state_done = _slots[old_top]._state_done.load(std::memory_order_acquire);




            // reserve a unique ticket
            const std::uint64_t ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t idx = static_cast<std::size_t>(ticket & _MASK);
            Slot& slot = _slots[idx];

            // try if the slot is currently empty (slot._ticket_index == ticket) for this ticket.
            if (slot._ticket_index.load(std::memory_order_acquire) != ticket) {
                // Reservation succeeded (++_top)
                // but the push is failed as the ring is full for this ticket.
                // Undoing the top incrementation is not possible without coordination
                // but the ticket is reserved and will be consumable once the slot cycles.
                return false;
            }

            // construct and publish
            ::new (slot.to_ptr()) T(std::forward<U>(data));
            slot._ticket_index.store(ticket + 1, std::memory_order_release);
            return true;
        }

        template <class U>
        void push_blocking(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            // reserve a unique ticket
            const std::uint64_t ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t idx = static_cast<std::size_t>(ticket & _MASK);
            Slot& slot = _slots[idx];

            // Spin until this slot is empty for this ticket (buffer index == ticket)
            while (slot._ticket_index.load(std::memory_order_acquire) != ticket) {}

            // construct and publish
            ::new (slot.to_ptr()) T(std::forward<U>(data));
            slot._ticket_index.store(ticket + 1, std::memory_order_release);
        }

        template <typename... Args>
        bool emplace_nonblocking(Args&&... args) {
            // reserve a unique ticket
            const std::uint64_t ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t idx = static_cast<std::size_t>(ticket & _MASK);
            Slot& slot = _slots[idx];

            // try if the slot is currently empty (slot._ticket_index == ticket) for this ticket.
            if (slot._ticket_index.load(std::memory_order_acquire) != ticket) {
                // Reservation succeeded (++_top)
                // but the push is failed as the ring is full for this ticket.
                // Undoing the top incrementation is not possible without coordination
                // but the ticket is reserved and will be consumable once the slot cycles.
                return false;
            }

            // construct and publish
            ::new (slot.to_ptr()) T(std::forward<Args>(args)...);
            slot._ticket_index.store(ticket + 1, std::memory_order_release);
            return true;
        }

        template <typename... Args>
        void emplace_blocking(Args&&... args) {
            // reserve a unique ticket
            const std::uint64_t ticket = _top.fetch_add(1, std::memory_order_acq_rel);
            const std::size_t idx = static_cast<std::size_t>(ticket & _MASK);
            Slot& slot = _slots[idx];

            // Spin until this slot is empty for this ticket (buffer index == ticket)
            while (slot._ticket_index.load(std::memory_order_acquire) != ticket) {}

            // construct and publish
            ::new (slot.to_ptr()) T(std::forward<Args>(args)...);
            slot._ticket_index.store(ticket + 1, std::memory_order_release);
        }

    public:

        // Initialize the buffer indices so that
        // the very first producer that reserves ticket t
        // sees slot._ticket_index == t which means an empty slot for that cycle.
        Stack() noexcept {
            for (std::uint64_t i = 0; i < capacity; ++i) {
                _slots[i]._ticket_index.store(i, std::memory_order_relaxed);
            }
        }

        // Assume single-threaded destruction.
        // Remove the remaining (not-yet-popped) elements
        // by inspecting if the buffer element at the buffer index is full.
        // Because _top is monotonic and the destrructor is assumed single-threaded,
        // it is safe to just attempt destruction when the slot is full.
        //     empty: buffer index % capacity == i
        //     full : buffer index % capacity == (i + 1) % capacity
        // No need to reset the buffer index sequence here in the destructor.
        ~Stack() {
            for (std::uint64_t i = 0; i < capacity; ++i) {
                auto ticket_index = _slots[i]._ticket_index.load(std::memory_order_relaxed);
                if (((ticket_index - 1) & _MASK) == i) _slots[i].to_ptr()->~T();
            }
        }

        // Non-copyable / non-movable for simplicity
        Stack(const Stack&) = delete;
        Stack& operator=(const Stack&) = delete;

        // non-blocking (no wait) push and emplace functions
        bool try_push(const T& data) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            return push_nonblocking(data);
        }
        bool try_push(T&& data) noexcept(std::is_nothrow_move_constructible_v<T>) {
            return push_nonblocking(std::move(data));
        }
        template <typename... Args>
        bool try_emplace(Args&&... args) {
            return emplace_nonblocking(std::forward<Args>(args)...);
        }

        // blocking (busy) push and emplace functions
        void push(const T& data) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            push_blocking(data);
        }
        void push(T&& data) noexcept(std::is_nothrow_move_constructible_v<T>) {
            push_blocking(std::move(data));
        }
        template <typename... Args>
        void emplace(Args&&... args) {
            emplace_blocking(std::forward<Args>(args)...);
        }

        // Pop the most recently pushed element (LIFO) after the producer finishes the publish.
        // Returns std::nullopt if stack appears empty at the reservation time.
        std::optional<T> pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            // Reserve the latest ticket by CAS-decrementing _top.
            // If _top == 0, stack is empty.
            std::uint64_t old_top = _top.load(std::memory_order_acquire);
            while (true) {
                if (old_top == 0) return std::nullopt;

                // Try to claim ticket old_top-1
                if (_top.compare_exchange_strong(
                        old_top,
                        old_top - 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // the ticket (old_top-1) is owned by this consumer
                    const std::uint64_t ticket = old_top - 1;
                    const std::size_t idx = static_cast<std::size_t>(ticket & _MASK);
                    Slot& slot = _slots[idx];

                    // Wait until producer finishes the publish (buffer_index == ticket + 1).
                    // Another consumer can't take this ticket because we've reserved it by CAS on _top.
                    while (slot._ticket_index.load(std::memory_order_acquire) != ticket + 1) {}

                    // Move the data and destroy in place
                    T* ptr = slot.to_ptr();
                    std::optional<T> data{std::move(*ptr)};
                    ptr->~T();

                    // Mark slot empty for the next cycle.
                    // After a full cycle of capacity tickets, the same index will be used again.
                    // The buffer index of the expected empty element for that future push will be ticket + capacity.
                    slot._ticket_index.store(ticket + capacity, std::memory_order_release);
                    return data;
                }
                // CAS failed: retry CAS with the updated expected value
            }
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
