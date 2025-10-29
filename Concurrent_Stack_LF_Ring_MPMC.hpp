// Concurrent_Stack_LF_Ring_MPMC
//
// The brute force solution for the lock-free/MPMC/ring stack problem:
//   Synchronize the top of the static ring buffer 
//     which is shared between producer and consumer threads.
//   Define an atomic state which allows managing the operation order
//     within/between the producers and the consumers.
//
// Atomic slot states: 
//   SPP: State-Producer-Progress: A producer is currently working on this slot
//   SPW: State-Producer-Waiting : A producer is currently waiting for a consumer to finish working on this slot
//   SPD: State-Producer-Done    : A producer has finished working on this slot
//   SCP: State-Consumer-Progress: A consumer is currently working on this slot
//   SCW: State-Consumer-Waiting : A consumer is currently waiting for a producer to finish working on this slot
//   SCD: State-Consumer-Done    : A consumer has finished working on this slot
//   
// Configuration:
//   Provides three options for each operation: busy, wait and try.
//     Busy Configuration:
//       Producers and consumers cycles the slots till they find a suitable slot
//       which allows the requested operation.
//       For example, a producer needs to find a slot with SCD state.
//       During the cycle, the producers increment the atomic top via fetch_add,
//       while the consumers decrements it via compare_exchange_strong(old, old - 1)
//     Wait Configuration:
//       In busy configuration, the threads can only claim aa slot
//       if its in DONE state of the counter thread type.
//       Hence, the producers look for SCD state, while the consumers for SPD state.
//       However, PROGRESS state is skipped.
//       In other words, while a thread owns a slot and works on the request,
//       the counter threads cannot claim the slot.
//       During this small window (after owning and before publishing period),
//       the counters are not allowed to access the slot and would skip it.
//       PROGRESS and WAITING states serve for this small window.
//       The counter thread does npt skip the slot
//       but updates the PROGRESS state as WAITING state
//       waiting for the counter to update the state as DONE.
//       Wait configuration allows the threads to operate on this small window.
//     Try Configuration:
//       Same as the busy configuration but does not loop through the slots
//       but returns a success flag for the current slot.
// 
// CAUTION:
//   Simpler conceptual model but not fully lock-free under heavy contention.
//   See Concurrent_Stack_LF_Ring_MPMC_optimized for ticket-based lock-free version.

#ifndef CONCURRENT_STACK_LF_RING_MPMC_HPP
#define CONCURRENT_STACK_LF_RING_MPMC_HPP

#include <cstddef>
#include <atomic>
#include <new>
#include <type_traits>
#include <optional>
#include "Concurrent_Stack.hpp"

namespace BA_Concurrency {
    // Slot states
    enum class Slot_States : uint8_t {
        SPP, // State-Producer-Progress
        SPW, // State-Producer-Waiting
        SPD, // State-Producer-Done
        SCP, // State-Consumer-Progress
        SCW, // State-Consumer-Waiting
        SCD  // State-Consumer-Done
    };

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
            std::atomic<std::uint8_t> _state{ Slot_States::SCD };
            T _data;
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

        // DOES NOT WAIT for the slots with SCP state
        // APPLY this operation IF the consumer PERFORMS A TIME CONSUMING process!
        //
        // Operation steps:
        //   Step 1: loop the slots while the following CAS fails:
        //             expected == SCD, desired = SPP
        //   Step 2: store the input data into the slot
        //   Step 3: store SPD into the state
        template <typename U = T>
        void busy_push(U&& data) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint8_t expected_state = Slot_States::SCD;
            while (
                !slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                expected_state = Slot_States::SCD;
                top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                slot = &_slots[top];
            };

            // Step 2
            slot->_data = std::forward<U>(data);

            // Step 3
            slot->_state.store(Slot_States::SPD, std::memory_order_release);
        }

        // See descriptions (info for each step) of busy_push
        template <typename... Args>
        void busy_emplace(Args&&... args) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint8_t expected_state = Slot_States::SCD;
            while (
                !slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                expected_state = Slot_States::SCD;
                top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                slot = &_slots[top];
            };

            // Step 2
            slot->_data = T(std::forward<Args>(args)...);

            // Step 3
            slot->_state.store(Slot_States::SPD, std::memory_order_release);
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
            std::uint8_t expected_state = Slot_States::SPD;
            while (
                !slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SCP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                expected_state = Slot_States::SPD;
                top = _top.compare_exchange_strong(
                    top,
                    top - 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed) & _MASK;
                slot = &_slots[top];
            };

            // Step 2
            slot->_state.store(Slot_States::SCD, std::memory_order_release);

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
        template <typename U = T>
        void wait_push(U&& data) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint8_t expected_state_1 = Slot_States::SCD;
            std::uint8_t expected_state_2 = Slot_States::SCP;
            while (true) {
                while (
                    !slot->_state.compare_exchange_strong(
                        expected_state_1,
                        Slot_States::SPP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed) &&
                    !slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SPW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    expected_state_1 = Slot_States::SCD;
                    expected_state_2 = Slot_States::SCP;
                    top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                    slot = &_slots[top];
                };

                // Step 2
                while (slot->_state.load(std::memory_order_acquire) == Slot_States::SPW);
                break;
            }

            // Step 3
            if (slot->_state.load(std::memory_order_acquire) == Slot_States::SCD)
                slot->_state.store(Slot_States::SPP, std::memory_order_release);

            // Step 4
            slot->_data = std::forward<U>(data);

            // Step 5
            slot->_state.store(Slot_States::SPD, std::memory_order_release);
        }

        // See descriptions (info for each step) of wait_push
        template <typename... Args>
        void wait_emplace(Args&&... args) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint8_t expected_state_1 = Slot_States::SCD;
            std::uint8_t expected_state_2 = Slot_States::SCP;
            while (true) {
                while (
                    !slot->_state.compare_exchange_strong(
                        expected_state_1,
                        Slot_States::SPP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed) &&
                    !slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SPW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    expected_state_1 = Slot_States::SCD;
                    expected_state_2 = Slot_States::SCP;
                    top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                    slot = &_slots[top];
                };

                // Step 2
                while (slot->_state.load(std::memory_order_acquire) == Slot_States::SPW);
                break;
            }

            // Step 3
            if (slot->_state.load(std::memory_order_acquire) == Slot_States::SCD)
                slot->_state.store(Slot_States::SPP, std::memory_order_release);

            // Step 4
            slot->_data = T(std::forward<Args>(args)...);

            // Step 5
            slot->_state.store(Slot_States::SPD, std::memory_order_release);
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
            std::uint8_t expected_state_1 = Slot_States::SPD;
            std::uint8_t expected_state_2 = Slot_States::SPP;
            while (true) {
                while (
                    !slot->_state.compare_exchange_strong(
                        expected_state_1,
                        Slot_States::SCP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed) &&
                    !slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SCW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    expected_state_1 = Slot_States::SPD;
                    expected_state_2 = Slot_States::SPP;
                    while(!_top.compare_exchange_strong(
                        top,
                        top - 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed) & _MASK);
                    slot = &_slots[top];
                };

                // Step 2
                while (slot->_state.load(std::memory_order_acquire) == Slot_States::SCW);
                break;
            }

            // Step 3
            slot->_state.store(Slot_States::SCD, std::memory_order_release);

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
        template <typename U = T>
        bool try_push(T&& data) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot& slot = _slots[top];

            // Step 1
            std::uint8_t expected_state = Slot_States::SCD;
            if (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) return false;

            // Step 2
            slot._data = std::forward<U>(data);

            // Step 3
            slot._state.store(Slot_States::SPD, std::memory_order_release);
            return true;
        }

        // See descriptions (info for each step) of try_push
        template <typename... Args>
        bool try_emplace(Args&&... args) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot& slot = _slots[top];

            // Step 1
            std::uint8_t expected_state = Slot_States::SCD;
            if (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) return false;

            // Step 2
            slot._data = T(std::forward<Args>(args)...);

            // Step 3
            slot._state.store(Slot_States::SPD, std::memory_order_release);
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
            std::uint8_t expected_state = Slot_States::SPD;
            if (
                !slot._state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SCP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) return std::nullopt;

            // Step 2
            slot._state.store(Slot_States::SCD, std::memory_order_release);

            // Step 3
            return std::move(slot._data);
        }
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using stack_LF_ring_MPMC = Concurrent_Stack<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_LF_RING_MPMC_HPP
