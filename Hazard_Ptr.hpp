// Hazard_Ptr.hpp
//
// Description:
//   The hazard pointers provides a solution for the destruction
//   of the shared nodes of a lock-free/linked data structure:
//     Pop operation needs to reclaim the memory for the head node.
//     However, the other consumer threads working on the same node
//     would have dangling pointers if the memory reclaim is not synchronized.
//     A hazard pointer solves this issue by protecting the registered object.
//     The memory can be reclaimed only when there exists no assigned hazard pointer.
//
// Requirements:
// - T must be noexcept-movable.
//
// Semantics:
//   push():
//     Follows the classical algorithm for the push:
//       1. Creates a new node.
//       2. Sets the next pointer of the new node to the current head.
//       3. Apply CAS on the head: CAS(new_node->head, new_node)
//   pop():
//     The classical pop routine is tuned
//     to reclaim the memory of the old head
//     under the protection of a hazard pointer:
//       1. Protect the head node by a hazard pointer
//       2. Apply CAS on the head: CAS(head, head->next)
//       3. Clear the hazard pointer
//       4. Move the data out from the old head node
//       5. Reclaim the memory for the old head:
//            old head will be destroyed if no other hazard is assigned to the node
//       6. Return the data
//
// See the documentation of Hazard_Ptr.hpp for the details about the hazard pointers.
//
// CAUTION:
//   The hazard pointers synchronize the memory reclamation
//   (i.e. the race condition related to the head pointer destruction at the end of a pop).
//   The race condition disappears when there exists a single consumer.
//   Hence, the usage of hazard pointers is
//   limited to the SPMC and MPMC configurations and
//   the repository lacks headers for
//   lock-free/linked/hazard solutions with SPSC and MPSC configurations.
//   For these two configurations refer to lock-free/linked solutions instead.
//   Hence, the list of headers for lock-free/linked solutions are:
//     Concurrent_Stack_LF_Linked_SPSC.hpp
//     Concurrent_Stack_LF_Linked_Hazard_SPMC.hpp
//     Concurrent_Stack_LF_Linked_XX_SPMC.hpp (e.g. reference counter)
//     Concurrent_Stack_LF_Linked_MPSC.hpp
//     Concurrent_Stack_LF_Linked_Hazard_MPMC.hpp
//     Concurrent_Stack_LF_Linked_XX_MPMC.hpp (e.g. reference counter)
//
// CAUTION:
//   use stack_LF_Linked_Hazard_MPMC alias at the end of this file
//   to get the right specialization of Concurrent_Stack
//   and to achieve the default arguments consistently.

#ifndef HAZARD_PTR_HPP
#define HAZARD_PTR_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <atomic>
#include <thread>
#include <vector>
#include <unordered_set>
#include <optional>
#include <algorithm>
#include <utility>
#include <type_traits>

namespace BA_Concurrency {
    inline constexpr std::size_t HAZARD_PTR_RECORD_COUNT__DEFAULT = 128;

    // a record for the hazard ptrs
    struct Hazard_Ptr_Record {
        std::atomic<std::thread::id> _owner_thread{};
        std::atomic<void*> _ptr{ nullptr };
    };

    // deferred memory reclamation wrapper
    struct Memory_Reclaimer {
        void *_ptr{};
        void(*_deleter)(void*){};
    };
    inline thread_local std::vector<Memory_Reclaimer> MEMORY_RECLAIMERS;

    // RAII class for the hazard ptrs
    template <std::size_t HAZARD_PTR_RECORD_COUNT = HAZARD_PTR_RECORD_COUNT__DEFAULT>
    class Hazard_Ptr_Owner {
        inline constexpr std::size_t RECLAIM_TRESHOLD = 64;
        static Hazard_Ptr_Record HAZARD_PTR_RECORDS[HAZARD_PTR_RECORD_COUNT];
        Hazard_Ptr_Record* _hazard_ptr_record;

        static Hazard_Ptr_Record* acquire_hazard_ptr_record() {
            auto this_tid = std::this_thread::get_id();

            // if already owned (re-entrant use in same thread)
            if (
                auto it = std::find_if(
                    HAZARD_PTR_RECORDS.begin(),
                    HAZARD_PTR_RECORDS.end(),
                    [&this_tid](const auto& hazard_ptr_record) {
                        return hazard_ptr_record._owner_thread.load(std::memory_order_acquire) == this_tid;
                    });
                it != HAZARD_PTR_RECORDS.end())
            {
                return &*it;
            }

            // find an unpublished hazard ptr record
            std::thread::id empty_tid{};
            for (auto& hazard_ptr_record : HAZARD_PTR_RECORDS)
                if (
                    hazard_ptr_record._owner_thread.compare_exchange_strong(
                        empty_tid,
                        this_tid,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                    return &hazard_ptr_record;
            // TODO:
            //   all hazard ptr records are in use.
            //   either increase HAZARD_PTR_RECORD_COUNT or use a dynamic registry.
            std::terminate();
        }

        // a helper function for the special functions.
        // reset the hazard ptr record to the default.
        void reset() {
            if (!_hazard_ptr_record) return;
            _hazard_ptr_record->_ptr.store(nullptr, std::memory_order_release);
            _hazard_ptr_record->_owner_thread.store(std::thread::id{}, std::memory_order_release);
            _hazard_ptr_record = nullptr;
        }

        // get all pointers protected by the hazard ptrs
        static std::unordered_set<void*> get_ptrs_protected_by_hazard_ptrs() {
            std::thread::id empty_tid{};
            std::unordered_set<void*> ptrs_protected_by_hazard_ptrs;
            ptrs_protected_by_hazard_ptrs.reserve(HAZARD_PTR_RECORD_COUNT);
            for (auto& hazard_ptr_record : HAZARD_PTR_RECORDS) {
                if (hazard_ptr_record._owner_thread.load(std::memory_order_acquire) != empty_tid) {
                    if (void* ptr = hazard_ptr_record._ptr.load(std::memory_order_acquire)) {
                        if (
                            std::find(
                                ptrs_protected_by_hazard_ptrs.cbegin(),
                                ptrs_protected_by_hazard_ptrs.cend(),
                                ptr) ==
                            ptrs_protected_by_hazard_ptrs.cend())
                        {
                            ptrs_protected_by_hazard_ptrs.insert(ptr);
                        }
                    }
                }
            }
            return ptrs_protected_by_hazard_ptrs;
        }

    public:

        Hazard_Ptr_Owner() : _hazard_ptr_record(acquire_hazard_ptr_record()) {}
        Hazard_Ptr_Owner(Hazard_Ptr_Owner&& rhs) noexcept
            : _hazard_ptr_record(rhs._hazard_ptr_record)
        {
            rhs._hazard_ptr_record = nullptr;
        }
        Hazard_Ptr_Owner& operator=(Hazard_Ptr_Owner&& rhs) noexcept {
            if (this != &rhs) {
                reset();
                _hazard_ptr_record = rhs._hazard_ptr_record;
                rhs._hazard_ptr_record = nullptr;
            }
            return *this;
        }
        Hazard_Ptr_Owner(const Hazard_Ptr_Owner&) = delete;
        Hazard_Ptr_Owner& operator=(const Hazard_Ptr_Owner&) = delete;
        ~Hazard_Ptr_Owner() { reset(); }

        // try to reclaim all memory blocks those are not protected by any hazard ptr
        static void try_reclaim_memory() {
            if (MEMORY_RECLAIMERS.empty()) return;

            auto ptrs_protected_by_hazard_ptrs = get_ptrs_protected_by_hazard_ptrs();
            std::vector<Memory_Reclaimer> memory_reclaimers__protected; // reclaimers with active hazard ptrs
            memory_reclaimers__protected.reserve(MEMORY_RECLAIMERS.size());
            for (auto& memory_reclaimer : MEMORY_RECLAIMERS) {
                if (
                    std::find(
                        ptrs_protected_by_hazard_ptrs.cbegin(),
                        ptrs_protected_by_hazard_ptrs.cend(),
                        memory_reclaimer._ptr) ==
                    ptrs_protected_by_hazard_ptrs.cend())
                {
                    memory_reclaimer._deleter(memory_reclaimer._ptr); // reclaim the memory
                } else {
                    memory_reclaimers__protected.push_back(memory_reclaimer);
                }
            }
            MEMORY_RECLAIMERS.swap(memory_reclaimers__protected); // reclaimers with active hazard ptrs
        }

        // add the ptr into the deferred reclamation list
        static inline void reclaim_memory_later(void* ptr, void (*deleter)(void*)) {
            MEMORY_RECLAIMERS.push_back(Memory_Reclaimer{ptr, deleter});
            if (MEMORY_RECLAIMERS.size() >= RECLAIM_TRESHOLD) try_reclaim_memory();
        }

        // protect a ptr with a hazard ptr
        void protect(void* ptr) const noexcept {
            _hazard_ptr_record->_ptr.store(ptr, std::memory_order_release);
        }

        // get the protected ptr
        void* get() const noexcept {
            return
                _hazard_ptr_record ?
                _hazard_ptr_record->_ptr.load(std::memory_order_acquire) :
                nullptr;
        }

        // remove the hazard ptr protection from the ptr
        void clear() const noexcept {
            _hazard_ptr_record->_ptr.store(nullptr, std::memory_order_release);
        }
    };
} // namespace BA_Concurrency

#endif // HAZARD_PTR_HPP
