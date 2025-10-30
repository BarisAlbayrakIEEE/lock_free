// Concurrent_Stack_LF_Ring_MPMC
//
// The brute force solution for the lock-free/MPMC/ring stack problem:
//   Synchronize the top of the static ring buffer 
//     which is shared between producer and consumer threads.
//   Define an atomic state which allows managing the operation order
//     within/between the producers and the consumers.
//
// Atomic slot states where PT and CT stand for producer and consumer threads respectively: 
//   SPP: State-Producer-Progress: PT owns the slot and is operating on it
//   SPW: State-Producer-Waiting : PT shares the slot ownership with a CT and waiting for the CT
//   SPD: State-Producer-Done    : PT release the slot ownership after publishing the data
//   SPR: State-Producer-Ready   : PT published the data and released the slot ownership to the waiting CT
//   SCP: State-Consumer-Progress: CT owns the slot and is operating on it
//   SCW: State-Consumer-Waiting : CT shares the slot ownership with a PT and waiting for the PT
//   SCD: State-Consumer-Done    : CT release the slot ownership after popping the data
//   SCR: State-Consumer-Ready   : CT popped the data and released the slot ownership to the waiting PT
//
// Example state transitions for a slot:
//   SCD->SPP->SPD->SCP->SCD
//   SCD->SPP->SCW->SPR->SCP->SPW->SCR->SPP
// 
// CAUTION:
//   This is a simple conceptual model for a lock-free/ring-buffer/MPMC stack problem
//   but actually not fully lock-free under heavy contention
//   as the single atomic top synchronization the producer incrementation and 
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

        // Loops the slots for busy push operation
        //
        // Operation steps:
        //   Step 1: double CAS loop: while(!CAS(SCD, SPP) && !CAS(SCP, SPW)) _top.fetch_add(1)
        //   Step 2: IF Step 1 results with SPW-> CAS loop: while(!CAS(SCR, SPP));
        //   Step 3: store the input data into the slot
        //   Step 4: CAS(SPP, SPD)
        //   Step 5: If Step 4 fails -> CAS(SCW, SPR)
        template <typename U = T>
        void push(U&& data) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            while (true) {
                std::uint8_t expected_state_1 = Slot_States::SCD;
                auto CAS1 = slot->_state.compare_exchange_strong(
                    expected_state_1,
                    Slot_States::SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
                std::uint8_t expected_state_2 = Slot_States::SCP;
                bool CAS2{};
                if (!CAS1)
                    CAS2 = slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SPW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                while (!CAS1 && !CAS2) {
                    top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                    slot = &_slots[top];

                    expected_state_1 = Slot_States::SCD;
                    CAS1 = slot->_state.compare_exchange_strong(
                        expected_state_1,
                        Slot_States::SPP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                    if (!CAS1) {
                        expected_state_2 = Slot_States::SCP;
                        CAS2 = slot->_state.compare_exchange_strong(
                            expected_state_2,
                            Slot_States::SPW,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed);
                    }
                };

                // Step 2
                if (CAS2) {
                    std::uint8_t expected_state_3 = Slot_States::SCR;
                    while(
                        !slot->_state.compare_exchange_strong(
                            expected_state_3,
                            Slot_States::SPP,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed));
                }
                break;
            }

            // Step 3
            slot->_data = std::forward<U>(data);

            // Step 4
            std::uint8_t expected_state = Slot_States::SPP;
            auto step4 = slot->_state.compare_exchange_strong(
                expected_state,
                Slot_States::SPD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 5
            if (!step4) {
                expected_state = Slot_States::SCW;
                slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPR,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)
            }
        }

        // Loops the slots for busy emplace operation
        //
        // Operation steps:
        //   Step 1: double CAS loop: while(!CAS(SCD, SPP) && !CAS(SCP, SPW)) _top.fetch_add(1)
        //   Step 2: IF Step 1 results with SPW-> CAS loop: while(!CAS(SCR, SPP));
        //   Step 3: store the input data into the slot
        //   Step 4: CAS(SPP, SPD)
        //   Step 5: If Step 4 fails -> CAS(SCW, SPR)
        template <typename... Args>
        void emplace(Args&&... args) noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            while (true) {
                std::uint8_t expected_state_1 = Slot_States::SCD;
                auto CAS1 = slot->_state.compare_exchange_strong(
                    expected_state_1,
                    Slot_States::SPP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
                std::uint8_t expected_state_2 = Slot_States::SCP;
                bool CAS2{};
                if (!CAS1)
                    CAS2 = slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SPW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                while (!CAS1 && !CAS2) {
                    top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                    slot = &_slots[top];

                    expected_state_1 = Slot_States::SCD;
                    CAS1 = slot->_state.compare_exchange_strong(
                        expected_state_1,
                        Slot_States::SPP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                    if (!CAS1) {
                        expected_state_2 = Slot_States::SCP;
                        CAS2 = slot->_state.compare_exchange_strong(
                            expected_state_2,
                            Slot_States::SPW,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed);
                    }
                };

                // Step 2
                if (CAS2) {
                    std::uint8_t expected_state_3 = Slot_States::SCR;
                    while(
                        !slot->_state.compare_exchange_strong(
                            expected_state_3,
                            Slot_States::SPP,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed));
                }
                break;
            }

            // Step 3
            slot->_data = T(std::forward<Args>(args)...);

            // Step 4
            std::uint8_t expected_state = Slot_States::SPP;
            auto step4 = slot->_state.compare_exchange_strong(
                expected_state,
                Slot_States::SPD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 5
            if (!step4) {
                expected_state = Slot_States::SCW;
                slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SPR,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)
            }
        }

        // Loops the slots for busy pop operation
        //
        // Operation steps:
        //   Step 1: double CAS loop: while(!CAS(SPD, SCP) && !CAS(SPP, SCW)) _top.fetch_add(1)
        //   Step 2: IF Step 1 results with SCW-> CAS loop: while(!CAS(SPR, SCP));
        //   Step 3: pop the value from the slot
        //   Step 4: CAS(SCP, SCD)
        //   Step 5: If Step 4 fails -> CAS(SPW, SCR)
        //   Step 6: return the popped value
        T pop() noexcept {
            std::uint64_t top = _top.load(std::memory_order_acquire) & _MASK;
            Slot *slot = &_slots[top];

            // Step 1
            while (true) {
                std::uint8_t expected_state_1 = Slot_States::SPD;
                auto CAS1 = slot->_state.compare_exchange_strong(
                    expected_state_1,
                    Slot_States::SCP,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed);
                std::uint8_t expected_state_2 = Slot_States::SPP;
                bool CAS2{};
                if (!CAS1)
                    CAS2 = slot->_state.compare_exchange_strong(
                        expected_state_2,
                        Slot_States::SCW,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                while (!CAS1 && !CAS2) {
                    top = _top.fetch_add(1, std::memory_order_acq_rel) & _MASK;
                    slot = &_slots[top];

                    expected_state_1 = Slot_States::SPD;
                    CAS1 = slot->_state.compare_exchange_strong(
                        expected_state_1,
                        Slot_States::SCP,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed);
                    if (!CAS1) {
                        expected_state_2 = Slot_States::SPP;
                        CAS2 = slot->_state.compare_exchange_strong(
                            expected_state_2,
                            Slot_States::SCW,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed);
                    }
                };

                // Step 2
                if (CAS2) {
                    std::uint8_t expected_state_3 = Slot_States::SPR;
                    while(
                        !slot->_state.compare_exchange_strong(
                            expected_state_3,
                            Slot_States::SCP,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed));
                }
                break;
            }

            // Step 3
            auto data = std::move(slot->_data);

            // Step 4
            std::uint8_t expected_state = Slot_States::SCP;
            auto step4 = slot->_state.compare_exchange_strong(
                expected_state,
                Slot_States::SCD,
                std::memory_order_acq_rel,
                std::memory_order_relaxed);

            // Step 5
            if (!step4) {
                expected_state = Slot_States::SPW;
                slot->_state.compare_exchange_strong(
                    expected_state,
                    Slot_States::SCR,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)
            }

            // Step 6
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
                    std::memory_order_relaxed)
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
                    std::memory_order_relaxed)
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
                    std::memory_order_relaxed)
            }

            // Step 5
            return std::move(data);
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
