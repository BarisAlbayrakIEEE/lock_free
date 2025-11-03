// Concurrent_Stack_LF_Ring_Brute_Force_MPMC.hpp
//
// Description:
//   The brute force solution for the lock-free/ring/MPMC stack problem:
//     Synchronize the top of the static ring buffer 
//       which is shared between producer and consumer threads.
//     Define an atomic state per buffer slot
//       which synchronizes the producers and consumers.
//
// Requirements:
// - capacity must be a power of two (for fast masking).
// - T must be noexcept-movable.
//
// Semantics:
//   Atomic slot states where PT and CT stand for producer and consumer threads respectively: 
//     SPP: State-Producer-Progress: PT owns the slot and is operating on it
//     SPW: State-Producer-Waiting : PT shares the slot ownership with a CT and waiting for the CT's notify
//     SPD: State-Producer-Done    : PT released the slot ownership after storing the data
//     SPR: State-Producer-Ready   : PT released the slot ownership to the waiting CT (notify CT) after storing the data
//     SCP: State-Consumer-Progress: CT owns the slot and is operating on it
//     SCW: State-Consumer-Waiting : CT shares the slot ownership with a PT and waiting for the PT's notify
//     SCD: State-Consumer-Done    : CT released the slot ownership after popping the data
//     SCR: State-Consumer-Ready   : CT released the slot ownership to the waiting PT (notify PT) after popping the data
//
//   push():
//     Loops through the slot buffer to reserve a slot
//     that has a suitable state for the operation:
//       CONSUMER/DONE (SCD) or CONSUMER/PROGRESS (SCP).
//     If the state is SCD (Case1), gets the ownership of the slot by CASing the state to:
//       PRODUCER/PROGRESS (SPP)
//     In Case1, writes the input data into the slot and publishes the data
//     by CASing the state to PRODUCER/DONE (SPD).
//     Otherwise (Case2), CASes the state of the slot to PRODUCER/WAITING (SPW).
//     Notice that, now, both the consumer and producer share the slot ownership.
//     This case simulates an exclusive shared ownership of a producer and a consumer
//     against the other threads operating on the stack.
//     From this point on, the operations on the slot are SPSC
//     which enables us to use notify semantics.
//     Waits until the consumer finishes its work and CASes the state to CONSUMER/READY (SCR):
//       slot->state.wait(SCR)
//     When finished its job, the consumer notifies the waiting producer by:
//       slot->state.notify_one(SCR)
//     When the state changes to SCR, the OS will wake up the producer.
//     When so, owns the slot by CASing the state to SPP.
//     Writes the input data into the slot and publishes the data
//     by CASing the state to SPD.
//   pop():
//     Pop is fully symmetric with push.
//     However, in order to describe the use of the state in more detail
//     I will go over the steps.
//
//     Loops through the slot buffer to reserve a slot
//     that has a suitable state for the operation: SPD or SPP.
//     If the state is SPD (Case1), gets the ownership of the slot by CASing the state to SCP.
//     In Case1, writes the input data into the slot and publishes the data
//     by CASing the state to SCD.
//     Otherwise (Case2), CASes the state of the slot to SCW.
//     Here, the SPSC window starts which enables us to use notify semantics.
//     Waits until the producer finishes its work and CASes the state to SPR:
//       slot->state.wait(SPR)
//     When finished its job, the producer notifies the waiting consumer by:
//       slot->state.notify_one(SPR)
//     When the state changes to SPR, the OS will wake up the producer.
//     When so, owns the slot by CASing the state to SCP.
//     Pops the data from the slot and CASes the state to SCD.
//
//   Try configurations (try_push and try_pop):
//     Producers and consumers need to loop through the slot buffer to reserve a slot
//     that has a suitable state for the operation.
//     In try configuration, the threads do not loop but
//     tries to execute the operation immediately on the slot pointed by the top counnter.
//     The operation is performed only if the top slot has a suitable state for the operation.
//
//   Example state transitions for a slot:
//     SCD->SPP->SPD->SCP->SCD:
//       no interference by counterpart threads while this thread is in progress (i.e. SPP and SCP)
//     SCD->SPP->SCW->SPR->SCP->SPW->SCR->SPP:
//       a counterpart thread interferes and starts waiting
//       during this thread is in progress (i.e. SPP and SCP)
//
// Progress:
//   Lock-free:
//     fully lock-free under low contention (i.e. capacity >> Nthread).
//   push():
//     back-pressures when the stack is full by spinning for the two states: SCD or SCP.
//   pop():
//     back-pressures when the stack is empty by spinning for the two states: SPD or SPP.
//
// Notes:
//   No dynamic allocation or reclamation:
//     ABA is avoided by the state flag.
//   Memory orders are chosen to
//   release data before the visibility of the state transitions and
//   to acquire data after observing the state transitions.
//
// CAUTION:
//   This is a simple conceptual model for a lock-free/ring-buffer/MPMC stack problem
//   but actually not fully lock-free under heavy contention (i.e. obstruction-free)
//   as the single atomic top synchronization allows
//   each thread to execute only in isolation.
//   Actually, this is one-to-one conversion of a single thread queue to a concurrent one:
//     a push increments the top index and a pop decrements it
//     where the synchronization for the top index is handled by a state flag.
//
// CAUTION:
//   In order to reduce the collision probability
//   (i.e. to switch from obstruction-free to lock-free),
//   the capacity of the buffer shall be increased.
//   Amprically, the following equality results well to achieve a lock-free execution:
//     capacity = 8 * N where N is the number of the threads
//
// CAUTION:
//   use stack_LF_ring_brute_force_MPMC alias at the end of this file
//   to get the right specialization of Concurrent_Stack
//   and to achieve the default arguments consistently.
//
// CAUTION:
//   See Concurrent_Stack_LF_Ring_Ticket_MPMC for ticket-based version
//   which guarantees lock-free execution regardless of the contention.

#ifndef CONCURRENT_STACK_LF_RING_BRUTE_FORCE_MPMC_HPP
#define CONCURRENT_STACK_LF_RING_BRUTE_FORCE_MPMC_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <new>
#include <optional>
#include "Concurrent_Stack.hpp"
#include "enum_ring_designs.hpp"
#include "aux_type_traits.hpp"

namespace BA_Concurrency {
    // Slot states
    // PT: producer thread
    // CT: consumer thread
    enum class Slot_States : uint8_t {
        SPP, // State-Producer-Progress: PT owns the slot and is operating on it
        SPW, // State-Producer-Waiting : PT shares the slot ownership with a CT and waiting for the CT
        SPD, // State-Producer-Done    : PT release the slot ownership after publishing the data
        SPR, // State-Producer-Ready   : PT published the data and released the slot ownership to the waiting CT
        SCP, // State-Consumer-Progress: CT owns the slot and is operating on it
        SCW, // State-Consumer-Waiting : CT shares the slot ownership with a PT and waiting for the PT
        SCD, // State-Consumer-Done    : CT release the slot ownership after popping the data
        SCR  // State-Consumer-Ready   : CT popped the data and released the slot ownership to the waiting PT
    };
    // Example state transitions for a slot:
    //   SCD->SPP->SPD->SCP->SCD
    //   SCD->SPP->SCW->SPR->SCP->SPW->SCR->SPP
    
    // use stack_LF_ring_brute_force_MPMC alias at the end of this file
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
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Brute_Force)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>
    {
        struct alignas(64) Slot {
            std::atomic<std::uint8_t> _state{ Slot_States::SCD };
            char pad[64 - sizeof(_state)]; // padding to avoid false sharing
            T _data;
        };

        // mask to modulo the ticket by capacity
        static constexpr std::size_t capacity = pow2_size<Capacity_As_Pow2>;
        static constexpr std::size_t _MASK = capacity - 1;

        // _top is the next ticket to push.
        alignas(64) std::atomic<std::uint64_t> _top{0};
        // the buffer of slots
        Slot _slots[capacity];

    public:

        // Non-copyable / non-movable for simplicity
        Concurrent_Stack(const Concurrent_Stack&) = delete;
        Concurrent_Stack& operator=(const Concurrent_Stack&) = delete;

        // Loops the slots for busy push operation
        //
        // Operation steps:
        //   Step 1: double CAS loop: while(!CAS(SCD, SPP) && !CAS(SCP, SPW)) _top.fetch_add(1)
        //   Step 2: IF Step 1 results with SPW -> slot->_state.wait() (to be notified for SCR)
        //   Step 3: IF Step 1 results with SPW -> CAS loop: while(!(SCR, SPP))
        //   Step 4: store the input data into the slot
        //   Step 5: CAS(SPP, SPD)
        //   Step 6: If Step 5 fails -> CAS(SCW, SPR)
        //   Step 7: Notify the waiting consumer for SCR
        template <typename U = T>
        void push(U&& data) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint8_t expected_state_1 = Slot_States::SCD;
            std::uint8_t expected_state_2 = Slot_States::SCP;
            bool CAS1{};
            bool CAS2{};
            while (!CAS1 && !CAS2) {
                CAS1 = slot->_state.compare_exchange_strong(
                    expected_state_1,
                    Slot_States::SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
                CAS2 = true;
                if (!CAS1) {
                    CAS2 = slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SPW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                }
                if (!CAS1 && !CAS2) {
                    top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                    slot = &_slots[top];
                    expected_state_1 = Slot_States::SCD;
                    expected_state_2 = Slot_States::SCP;
                }
            };

            if (!CAS1 && CAS2) {
                // Step 2
                std::uint8_t expected_state_3 = Slot_States::SCR;
                slot->_state.wait(expected_state_3, std::memory_order_acquire);

                // Step 3
                std::uint8_t expected_state_4 = Slot_States::SCR;
                while (
                    !slot->_state.compare_exchange_weak(
                        expected_state_4,
                        Slot_States::SPP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) expected_state_4 = Slot_States::SCR;
            }

            // Step 4
            slot->_data = std::forward<U>(data);

            // Step 5
            std::uint8_t expected_state = Slot_States::SPP;
            auto step5 = slot->_state.compare_exchange_strong(
                expected_state,
                Slot_States::SPD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 6
            if (!step5) {
                expected_state = Slot_States::SCW;
                slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPR,
                    std::memory_order_release,
                    std::memory_order_relaxed);

                // Step 7
                slot->_state.notify_one();
            }
        }

        // Loops the slots for busy emplace operation
        //
        // Operation steps:
        //   Step 1: double CAS loop: while(!CAS(SCD, SPP) && !CAS(SCP, SPW)) _top.fetch_add(1)
        //   Step 2: IF Step 1 results with SPW -> slot->_state.wait() (to be notified for SCR)
        //   Step 3: IF Step 1 results with SPW -> CAS loop: while(!(SCR, SPP))
        //   Step 4: inplace construct the object in the slot
        //   Step 5: CAS(SPP, SPD)
        //   Step 6: If Step 5 fails -> CAS(SCW, SPR)
        //   Step 7: Notify the waiting consumer for SCR
        template <typename... Args>
        void emplace(Args&&... args) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint8_t expected_state_1 = Slot_States::SCD;
            std::uint8_t expected_state_2 = Slot_States::SCP;
            bool CAS1{};
            bool CAS2{};
            while (!CAS1 && !CAS2) {
                CAS1 = slot->_state.compare_exchange_strong(
                    expected_state_1,
                    Slot_States::SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
                CAS2 = true;
                if (!CAS1) {
                    CAS2 = slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SPW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                }
                if (!CAS1 && !CAS2) {
                    top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                    slot = &_slots[top];
                    expected_state_1 = Slot_States::SCD;
                    expected_state_2 = Slot_States::SCP;
                }
            };

            if (!CAS1 && CAS2) {
                // Step 2
                std::uint8_t expected_state_3 = Slot_States::SCR;
                slot->_state.wait(expected_state_3, std::memory_order_acquire);

                // Step 3
                std::uint8_t expected_state_4 = Slot_States::SCR;
                while (
                    !slot->_state.compare_exchange_weak(
                        expected_state_4,
                        Slot_States::SPP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) expected_state_4 = Slot_States::SCR;
            }

            // Step 4
            ::new (&slot->_data) T(std::forward<Args>(args)...);

            // Step 5
            std::uint8_t expected_state = Slot_States::SPP;
            auto step5 = slot->_state.compare_exchange_strong(
                expected_state,
                Slot_States::SPD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 6
            if (!step5) {
                expected_state = Slot_States::SCW;
                slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPR,
                    std::memory_order_release,
                    std::memory_order_relaxed);

                // Step 7
                slot->_state.notify_one();
            }
        }

        // Loops the slots for busy pop operation
        //
        // Operation steps:
        //   Step 1: double CAS loop: while(!CAS(SPD, SCP) && !CAS(SPP, SCW)) _top.fetch_add(1)
        //   Step 2: IF Step 1 results with SCW -> slot->_state.wait() (to be notified for SPR)
        //   Step 3: IF Step 1 results with SCW -> CAS loop: while(!(SPR, SCP))
        //   Step 4: pop the value from the slot
        //   Step 5: CAS(SCP, SCD)
        //   Step 6: If Step 5 fails -> CAS(SPW, SCR)
        //   Step 7: Notify the waiting producer for SPR
        //   Step 8: return the popped value
        T pop() noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            std::uint8_t expected_state_1 = Slot_States::SPD;
            std::uint8_t expected_state_2 = Slot_States::SPP;
            bool CAS1{};
            bool CAS2{};
            while (!CAS1 && !CAS2) {
                CAS1 = slot->_state.compare_exchange_strong(
                    expected_state_1,
                    Slot_States::SCP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
                CAS2 = true;
                if (!CAS1) {
                    CAS2 = slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SCW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                }
                if (!CAS1 && !CAS2) {
                    _top.compare_exchange_strong(
                        top,
                        top - 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                    top &= _MASK;
                    slot = &_slots[top];
                    expected_state_1 = Slot_States::SPD;
                    expected_state_2 = Slot_States::SPP;
                }
            };

            if (!CAS1 && CAS2) {
                // Step 2
                std::uint8_t expected_state_3 = Slot_States::SPR;
                slot->_state.wait(expected_state_3, std::memory_order_acquire);

                // Step 3
                std::uint8_t expected_state_4 = Slot_States::SPR;
                while (
                    !slot->_state.compare_exchange_weak(
                        expected_state_4,
                        Slot_States::SCP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) expected_state_4 = Slot_States::SPR;
            }

            // Step 4
            auto data = std::move(slot->_data);

            // Step 5
            std::uint8_t expected_state = Slot_States::SCP;
            auto step5 = slot->_state.compare_exchange_strong(
                expected_state,
                Slot_States::SCD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 6
            if (!step5) {
                expected_state = Slot_States::SPW;
                slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SCR,
                    std::memory_order_release,
                    std::memory_order_relaxed);

                // Step 7
                slot->_state.notify_one();
            }

            // Step 8
            return std::move(data);
        }

        // Returns immediately the result of the push on the current top slot
        // without seeking for a suitable slot
        //
        // Operation steps:
        //   Step 1: return false if: !CAS(SCD, SPP)
        //   Step 2: store the input data into the slot
        //   Step 3: CAS(SPP, SPD)
        //   Step 4: If Step 3 fails -> CAS(SCW, SPR)
        //   Step 5: return true
        template <typename U = T>
        bool try_push(U&& data) noexcept {
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
            expected_state = Slot_States::SPP;
            auto step3 = slot._state.compare_exchange_strong(
                expected_state,
                Slot_States::SPD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 4
            if (!step3) {
                expected_state = Slot_States::SCW;
                slot._state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPR,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
            }

            // Step 5
            return true;
        }

        // Returns immediately the result of the emplace on the current top slot
        // without seeking for a suitable slot
        //
        // Operation steps:
        //   Step 1: return false if: !CAS(SCD, SPP)
        //   Step 2: store the input data into the slot
        //   Step 3: CAS(SPP, SPD)
        //   Step 4: If Step 3 fails -> CAS(SCW, SPR)
        //   Step 5: return true
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
            expected_state = Slot_States::SPP;
            auto step3 = slot._state.compare_exchange_strong(
                expected_state,
                Slot_States::SPD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 4
            if (!step3) {
                expected_state = Slot_States::SCW;
                slot._state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPR,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
            }

            // Step 5
            return true;
        }

        // Returns immediately the result of the push on the current top slot
        // without seeking for a suitable slot
        //
        // Operation steps:
        //   Step 1: return false if: !CAS(SPD, SCP)
        //   Step 2: pop the value from the slot
        //   Step 3: CAS(SCP, SCD)
        //   Step 4: If Step 3 fails -> CAS(SPW, SCR)
        //   Step 5: return the popped value
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
            auto data = std::move(slot._data);

            // Step 3
            expected_state = Slot_States::SCP;
            auto step3 = slot._state.compare_exchange_strong(
                expected_state,
                Slot_States::SCD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 4
            if (!step3) {
                expected_state = Slot_States::SPW;
                slot._state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SCR,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
            }

            // Step 5
            return std::move(data);
        }
    };

    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    using stack_LF_ring_brute_force_MPMC = Concurrent_Stack<
        true,
        Enum_Structure_Types::Static_Ring_Buffer,
        Enum_Concurrency_Models::MPMC,
        T,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Ring_Designs::Brute_Force)>,
        std::integral_constant<unsigned char, Capacity_As_Pow2>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_LF_RING_BRUTE_FORCE_MPMC_HPP
