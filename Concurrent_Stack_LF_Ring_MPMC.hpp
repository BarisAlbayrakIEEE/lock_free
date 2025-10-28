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
        // Slot states
        static const std::uint64_t SPP = 0; // State-Producer-Progress
        static const std::uint64_t SPW = 1; // State-Producer-Waiting
        static const std::uint64_t SPD = 2; // State-Producer-Done
        static const std::uint64_t SCP = 3; // State-Consumer-Progress
        static const std::uint64_t SCW = 4; // State-Consumer-Waiting
        static const std::uint64_t SCD = 5; // State-Consumer-Done

        struct alignas(64) Slot {
            std::atomic<std::uint64_t> _state;
            alignas(64) T _data;
            Slot() noexcept : _state(state_consumer_done) {}; // ready for push
        };

        // mask to modulo the ticket by capacity
        static constexpr std::size_t capacity = std::size_t(1) << Capacity_As_Pow2;
        static constexpr std::size_t _MASK = capacity - 1;

        // _top is the next ticket to push.
        alignas(64) std::atomic<std::uint64_t> _top{0};
        // the buffer of slots
        Slot _slots[capacity];

        // DOES NOT WAIT for the slots with SCP state
        // APPLY this operation IF the consumer PERFORMS A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: loop the slots while the following CAS fails:
        //             expected == SCD, desired = SPP
        //   Step 2: store SPP into the state
        //   Step 3: store the input data into the slot
        //   Step 4: store SPD into the state
        template <class U = T>
        void busy_push_helper(U&& data) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint64_t expected_state = SCD;
            while (
                !slot->_state.compare_exchange_strong(
                    expected_state,
                    SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                expected_state = SCD;
                ++top;
                top &=  _MASK;
                _top.store(top, std::memory_order_release);
                slot = &_slots[top];
            };

            // Step 2
            slot->_state.store(SPP, std::memory_order_release);

            // Step 3
            slot->_data = std::forward<U>(data);

            // Step 4
            slot->_state.store(SPD, std::memory_order_release);
        }

        // WAITS for the slots with SCP state
        // APPLY this operation IF the consumer DOESN'T PERFORM A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: loop the slots while the following two CASs fail:
        //             (expected == SCD, desired = SPP) and (expected == SCP, desired = SPW)
        //   Step 2: IF Step 1 results with SPW, wait till SCD
        //   Step 3: store SPP into the state
        //   Step 4: store the input data into the slot
        //   Step 5: store SPD into the state
        template <class U = T>
        void wait_push_helper(U&& data) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint64_t expected_state_1 = SCD;
            std::uint64_t expected_state_2 = SCP;
            while (true) {
                while (
                    !slot->_state.compare_exchange_strong(
                        expected_state_1,
                        SPP,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire) &&
                    !slot->_state.compare_exchange_strong(
                        expected_state_2,
                        SPW,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    expected_state_1 = SCD;
                    expected_state_2 = SCP;
                    ++top;
                    top &=  _MASK;
                    _top.store(top, std::memory_order_release);
                    slot = &_slots[top];
                };

                // Step 2
                while (slot->_state.load(std::memory_order_acquire) == SPW);
            }

            // Step 3
            slot->_state.store(SPP, std::memory_order_release);

            // Step 4
            slot->_data = std::forward<U>(data);

            // Step 5
            slot->_state.store(SPD, std::memory_order_release);
        }

        // DOES NOT WAIT for the slots with SCP state
        // APPLY this operation IF the consumer PERFORMS A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: return if the following CAS fails:
        //             expected == SCD, desired = SPP
        //   Step 2: store the input data into the slot
        //   Step 3: store SPD into the state
        template <class U = T>
        bool try_push_helper(U&& data) noexcept {
            // Try to claim a slot with consumer/done state
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot& slot = _slots[top];

            // Step 1
            std::uint64_t expected_state = SCD;
            if (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) return false;

            // Step 2
            slot->_data = std::forward<U>(data);

            // Step 3
            slot._state.store(SPD, std::memory_order_release);
            return true;
        }

    public:

        // Non-copyable / non-movable for simplicity
        Stack(const Stack&) = delete;
        Stack& operator=(const Stack&) = delete;

        // DOES NOT WAIT for the slots with SCP state
        // APPLY this operation IF the consumer PERFORMS A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: loop the slots while the following CAS fails:
        //             expected == SCD, desired = SPP
        //   Step 2: store SPP into the state
        //   Step 3: store the input data into the slot
        //   Step 4: store SPD into the state
        inline void busy_push(const T& data) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            busy_push_helper(data);
        }
        inline void busy_push(T&& data) noexcept(std::is_nothrow_move_constructible_v<T>) {
            busy_push_helper(std::move(data));
        }

        // See descriptions (info for each step) of busy_push
        template <typename... Args>
        void busy_emplace(Args&&... args) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint64_t expected_state = SCD;
            while (
                !slot->_state.compare_exchange_strong(
                    expected_state,
                    SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                expected_state = SCD;
                ++top;
                top &=  _MASK;
                _top.store(top, std::memory_order_release);
                slot = &_slots[top];
            };

            // Step 2
            slot->_state.store(SPP, std::memory_order_release);

            // Step 3
            slot->_data = T(std::forward<Args>(args)...);

            // Step 4
            slot->_state.store(SPD, std::memory_order_release);
        }

        // DOES NOT WAIT for the slots with SPP state
        // APPLY this operation IF the producer PERFORMS A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: loop the slots while the following CAS fails:
        //             expected == SPD, desired = SCP
        //   Step 2: store SCD into the state
        //   Step 3: return the value
        T busy_pop() noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint64_t expected_state = SPD;
            while (
                !slot->_state.compare_exchange_strong(
                    expected_state,
                    SCP,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                expected_state = SPD;
                if (!top) top = _MASK;
                else --top;
                top &=  _MASK;
                _top.store(top, std::memory_order_release);
                slot = &_slots[top];
            };

            // Step 2
            slot->_state.store(SCD, std::memory_order_release);

            // Step 3
            return std::move(slot->_data);
        }

        // WAITS for the slots with SCP state
        // APPLY this operation IF the consumer DOESN'T PERFORM A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: loop the slots while the following two CASs fail:
        //             (expected == SCD, desired = SPP) and (expected == SCP, desired = SPW)
        //   Step 2: IF Step 1 results with SPW, wait till SCD
        //   Step 3: store SPP into the state
        //   Step 4: store the input data into the slot
        //   Step 5: store SPD into the state
        inline void wait_push(const T& data) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            wait_push_helper(data);
        }
        inline void wait_push(T&& data) noexcept(std::is_nothrow_move_constructible_v<T>) {
            wait_push_helper(std::move(data));
        }

        // See descriptions (info for each step) of wait_push
        template <typename... Args>
        void wait_emplace(Args&&... args) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint64_t expected_state_1 = SCD;
            std::uint64_t expected_state_2 = SCP;
            while (true) {
                while (
                    !slot->_state.compare_exchange_strong(
                        expected_state_1,
                        SPP,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire) &&
                    !slot->_state.compare_exchange_strong(
                        expected_state_2,
                        SPW,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    expected_state_1 = SCD;
                    expected_state_2 = SCP;
                    ++top;
                    top &=  _MASK;
                    _top.store(top, std::memory_order_release);
                    slot = &_slots[top];
                };

                // Step 2
                while (slot->_state.load(std::memory_order_acquire) == SPW);
            }

            // Step 3
            slot->_state.store(SPP, std::memory_order_release);

            // Step 4
            slot->_data = T(std::forward<Args>(args)...);

            // Step 5
            slot->_state.store(SPD, std::memory_order_release);
        }

        // WAITS for the slots with SPP state
        // APPLY this operation IF the producer DOESN'T PERFORM A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: loop the slots while the following two CASs fail:
        //             (expected == SPD, desired = SCP) and (expected == SPP, desired = SCW)
        //   Step 2: IF Step 1 results with SCW, wait till SPD
        //   Step 3: store SCD into the state
        //   Step 4: return the value
        T wait_pop() noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint64_t expected_state_1 = SPD;
            std::uint64_t expected_state_2 = SPP;
            while (true) {
                while (
                    !slot->_state.compare_exchange_strong(
                        expected_state_1,
                        SCP,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire) &&
                    !slot->_state.compare_exchange_strong(
                        expected_state_2,
                        SCW,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    expected_state_1 = SPD;
                    expected_state_2 = SPP;
                    if (!top) top = _MASK;
                    else --top;
                    top &=  _MASK;
                    _top.store(top, std::memory_order_release);
                    slot = &_slots[top];
                };

                // Step 2
                while (slot->_state.load(std::memory_order_acquire) == SCW);
            }

            // Step 3
            slot->_state.store(SCD, std::memory_order_release);

            // Step 4
            return std::move(slot->_data);
        }

        // DOES NOT WAIT for the slots with SCP state
        // APPLY this operation IF the consumer PERFORMS A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: return if the following CAS fails:
        //             expected == SCD, desired = SPP
        //   Step 2: store the input data into the slot
        //   Step 3: store SPD into the state
        inline bool try_push(const T& data) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            return try_push_helper(data);
        }
        inline bool try_push(T&& data) noexcept(std::is_nothrow_move_constructible_v<T>) {
            return try_push_helper(std::move(data));
        }

        // See descriptions (info for each step) of try_push
        template <typename... Args>
        bool try_emplace(Args&&... args) noexcept {
            // Try to claim a slot with consumer/done state
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot& slot = _slots[top];

            // Step 1
            std::uint64_t expected_state = SCD;
            if (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) return false;

            // Step 2
            slot->_data = T(std::forward<Args>(args)...);

            // Step 3
            slot._state.store(SPD, std::memory_order_release);
            return true;
        }

        // DOES NOT WAIT for the slots with SPP state
        // APPLY this operation IF the producer PERFORMS A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: return if the following CAS fails:
        //             expected == SPD, desired = SCP
        //   Step 2: store SCD into the state
        //   Step 3: return the value
        std::optional<T> try_pop() noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot& slot = _slots[top];

            // Step 1
            std::uint64_t expected_state = SPD;
            if (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    SCP,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return std::nullopt;
            };

            // Step 2
            slot._state.store(SCD, std::memory_order_release);

            // Step 3
            return std::move(slot._data);
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
