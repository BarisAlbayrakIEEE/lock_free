// Hazard_Ptr.hpp
//
// Description:
//   The hazard pointers allows safe destruction of the shared objects
//   during lock-free execution:
//     1. Assign a hazard ptr to the object ptr to prevent destruction of the object by another thread,
//     2. Get the ownership of the object ptr under the protection of the hazard ptr,
//     3. Remove the protection of the hazard ptr (i.e. reset the hazard ptr),
//     4. Add the object ptr to the list of the objects to be reclaimed later.
//
//   The memory reclamation is performed on the defered list of reclaimers
//   after the number of the reclaimers reaches a treshold value.
//
// Requirements:
// - T must be noexcept-movable.
//
// Design:
//   Types:
//     Hazard_Ptr_Record:
//       Defines the hazard ptr with two atomic members (safe synchronized access):
//         _owner_thread: the thread requesting the hazard ptr
//         _ptr         : the ptr to be protected by the hazard ptr
//     Memory_Reclaimer:
//       Defines the memory reclamation logic with two members:
//         _ptr    : the ptr for which the pointee will be deleted
//         _deleter: the function which deletes the pointee
//     Hazard_Ptr_Owner:
//       RAII class for hazard ptrs associating a hazard ptr record:
//         _hazard_ptr_record
//       Provides the hazard ptr interface:
//         protect(void *ptr):
//           Publishes the associated hazard ptr record with the input "ptr"
//         clear():
//           Resets the associated hazard ptr record to
//           the default constructed thread id and nullptr
//
//       Defines the pool of the hazard ptr records which is shared by all threads:
//         static Hazard_Ptr_Record HAZARD_PTR_RECORDS[HAZARD_PTR_RECORD_COUNT]
//
//   Additionally, a thread local pool of memory reclaimers
//   allows accessing the memory reclaimers
//   without the need for the synchronization primitives:
//     inline thread_local std::vector<Memory_Reclaimer> MEMORY_RECLAIMERS;
//
//   The memory reclaimer pool can be defined global by a lock-free linked list.
//   However, this would add more atomic operations into the execution paths
//   which would decrease the performance.
//
// Semantics:
//   0. All hazard ptr records in Hazard_Ptr_Owner::HAZARD_PTR_RECORDS are initialized with:
//        default constructed thread id and nullptr
//   1. Hazard_Ptr_Owner::protect(void *ptr) method acquires a default constructed hazard ptr record
//      from the shared Hazard_Ptr_Owner::HAZARD_PTR_RECORDS.
//      Notice that, although HAZARD_PTR_RECORDS is not protected for synchronous access
//      the contained hazard ptr records are.
//      Hence, this operation is safe.
//      Then, the hazard ptr record is published
//      with the id of the requesting thread and the input ptr.
//   2. Hazard_Ptr_Owner::clear method resets the associated hazard ptr record
//      to the default constructed thread id and nullptr.
//   3. Static Hazard_Ptr_Owner::reclaim_memory_later function
//      pushes a new memory reclaimer into the thread_local MEMORY_RECLAIMERS.
//   4. Static Hazard_Ptr_Owner::try_reclaim_memory function
//      compares the thread local MEMORY_RECLAIMERS with the shared HAZARD_PTR_RECORDS
//      and reclaim memory for each entry in MEMORY_RECLAIMERS
//      which is not involved in HAZARD_PTR_RECORDS.
//      In other words, per thread base, memory reclaim is performed
//      for each memory reclaimer which holds a ptr not protected by any hazard ptr.
//      In other words, in mathematical convention, the below set is reclaimed:
//        RECLAIMERS_WITH_NOT_PROTECTED_PTRS = MEMORY_RECLAIMERS - HAZARD_PTR_RECORDS
//      This function is called by reclaim_memory_later
//      when the number of the size of MEMORY_RECLAIMERS reaches the treshold value
//      which is set as the half of HAZARD_PTR_RECORD_COUNT template parameter.

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
        void *_context{};
        void(*_deleter)(void*, void*){};
    };

    // list of Memory_Reclaimers:
    //   a thread_local container is used as,
    //   otherwise, it would require a synchronization (e.g. a lock-free list).
    //   Hence, memory reclamation is performed per thread
    //   on a separate list of objects to be deleted.
    //   The list of the hazard ptrs (i.e. Hazard_Ptr_Owner<N>::HAZARD_PTR_RECORDS),
    //   on the other hand, are shared and accessed synchronously based on the thread id.
    inline thread_local std::vector<Memory_Reclaimer> MEMORY_RECLAIMERS;

    // RAII class for the hazard ptrs
    template <std::size_t HAZARD_PTR_RECORD_COUNT = HAZARD_PTR_RECORD_COUNT__DEFAULT>
    class Hazard_Ptr_Owner {
        inline constexpr std::size_t RECLAIM_THRESHOLD = HAZARD_PTR_RECORD_COUNT / 2;

        // The list of the hazard ptrs are shared
        // Hazard ptrs are accessed synchronously based on the thread id.
        static Hazard_Ptr_Record HAZARD_PTR_RECORDS[HAZARD_PTR_RECORD_COUNT];

        // The hazard ptr record is managed by the two member functions:
        //   protect and clear
        Hazard_Ptr_Record* _hazard_ptr_record;

        // get an unpublished hazard pointer
        // CAUTION: See TODO comment at the end of the function
        static Hazard_Ptr_Record* acquire_hazard_ptr_record() {
            auto this_tid = std::this_thread::get_id();

            // if already owned (re-entrant use in same thread)
            auto ite{ std::end(HAZARD_PTR_RECORDS) };
            if (
                auto it = std::find_if(
                    std::begin(HAZARD_PTR_RECORDS),
                    ite,
                    [&this_tid](const auto& hazard_ptr_record) {
                        return hazard_ptr_record._owner_thread.load(std::memory_order_acquire) == this_tid;
                    });
                it != ite)
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
                        if (ptrs_protected_by_hazard_ptrs.find(ptr) == ptrs_protected_by_hazard_ptrs.cend()) {
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

        // get the protected ptr
        void* get() const noexcept {
            return
                _hazard_ptr_record ?
                _hazard_ptr_record->_ptr.load(std::memory_order_acquire) :
                nullptr;
        }

        // protect a ptr with a hazard ptr
        void protect(void* ptr) const noexcept {
            _hazard_ptr_record->_ptr.store(ptr, std::memory_order_release);
        }

        // remove the hazard ptr protection from the ptr
        void clear() const noexcept {
            _hazard_ptr_record->_ptr.store(nullptr, std::memory_order_release);
        }

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
                    memory_reclaimer._deleter(memory_reclaimer._ptr, memory_reclaimer._context); // reclaim the memory
                } else {
                    memory_reclaimers__protected.push_back(memory_reclaimer);
                }
            }
            MEMORY_RECLAIMERS.swap(memory_reclaimers__protected); // reclaimers with active hazard ptrs
        }

        // add the ptr into the deferred reclamation list
        static inline void reclaim_memory_later(void *ptr, void *context, void (*deleter)(void*, void*)) {
            MEMORY_RECLAIMERS.push_back(Memory_Reclaimer{ptr, deleter});
            if (MEMORY_RECLAIMERS.size() >= RECLAIM_THRESHOLD) try_reclaim_memory();
        }
    };
} // namespace BA_Concurrency

#endif // HAZARD_PTR_HPP
