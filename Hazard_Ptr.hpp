#ifndef HAZARD_PTR_HPP
#define HAZARD_PTR_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <atomic>
#include <thread>
#include <vector>
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
        void* _ptr{};
        void(*_deleter)(void*){};
    };
    inline thread_local std::vector<Memory_Reclaimer> MEMORY_RECLAIMERS;

    // RAII class for the hazard ptrs
    template <std::size_t HAZARD_PTR_RECORD_COUNT = HAZARD_PTR_RECORD_COUNT__DEFAULT>
    class Hazard_Ptr_Owner {
        Hazard_Ptr_Record* _hazard_ptr_record{};

        static Hazard_Ptr_Record* acquire_hazard_ptr_record() {
            auto this_tid = std::this_thread::get_id();
            for (auto& hazard_ptr_record : HAZARD_PTR_RECORDS) {
                std::thread::id old_tid{};
                if (
                    hazard_ptr_record._owner_thread.compare_exchange_strong(
                        old_tid,
                        this_tid,
                        std::memory_order_acq_rel))
                    return &hazard_ptr_record;

                // if already owned (re-entrant use in same thread)
                if (hazard_ptr_record._owner_thread.load(std::memory_order_acquire) == this_tid)
                    return &hazard_ptr_record;
            }
            // TODO: Out of hazard slots: either increase HAZARD_PTR_RECORD_COUNT or use a dynamic registry.
            std::terminate();
        }

        void reset() {
            if (!_hazard_ptr_record) return;
            _hazard_ptr_record->_ptr.store(nullptr, std::memory_order_release);
            _hazard_ptr_record->_owner_thread.store(std::thread::id{}, std::memory_order_release);
            _hazard_ptr_record = nullptr;
        }

    public:

        inline constexpr std::size_t RECLAIM_TRESHOLD = 64;
        static inline Hazard_Ptr_Record HAZARD_PTR_RECORDS[HAZARD_PTR_RECORD_COUNT];

        // collect all currently published hazard ptrs into a vector
        static std::vector<void*> snapshot_hazard_ptrs() {
            std::vector<void*> hazard_ptrs;
            hazard_ptrs.reserve(HAZARD_PTR_RECORD_COUNT);
            std::thread::id empty_tid{};
            for (auto& hazard_ptr_record : HAZARD_PTR_RECORDS) {
                if (hazard_ptr_record._owner_thread.load(std::memory_order_acquire) != empty_tid) {
                    if (void* ptr = hazard_ptr_record._ptr.load(std::memory_order_acquire)) {
                        hazard_ptrs.push_back(ptr);
                    }
                }
            }
            return hazard_ptrs;
        }

        // try to reclaim all memory blocks those are not protected by any hazard ptr
        static void try_reclaim_memory() {
            if (MEMORY_RECLAIMERS.empty()) return;

            auto hazard_ptrs = snapshot_hazard_ptrs();
            std::vector<Memory_Reclaimer> memory_reclaimers__protected;
            memory_reclaimers__protected.reserve(MEMORY_RECLAIMERS.size());
            for (auto& memory_reclaimer : MEMORY_RECLAIMERS) {
                if (
                    std::find(
                        hazard_ptrs.begin(),
                        hazard_ptrs.end(),
                        memory_reclaimer._ptr) ==
                    hazard_ptrs.end())
                {
                    memory_reclaimer._deleter(memory_reclaimer._ptr);
                } else {
                    memory_reclaimers__protected.push_back(memory_reclaimer);
                }
            }
            MEMORY_RECLAIMERS.swap(memory_reclaimers__protected);
        }

        // add the ptr into the deferred reclamation list
        static inline void reclaim_memory_later(void* ptr, void (*deleter)(void*)) {
            MEMORY_RECLAIMERS.push_back(Memory_Reclaimer{ptr, deleter});
            if (MEMORY_RECLAIMERS.size() >= RECLAIM_TRESHOLD) try_reclaim_memory();
        }

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

        // clear the hazard ptr protection from the ptr
        void clear() const noexcept {
            _hazard_ptr_record->_ptr.store(nullptr, std::memory_order_release);
        }
    };
} // namespace BA_Concurrency

#endif // HAZARD_PTR_HPP
