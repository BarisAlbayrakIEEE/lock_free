// lock_free_stack_hazard.hpp

#ifndef STACK_LF_LINKED_MPMC_NOWAIT_HPP
#define STACK_LF_LINKED_MPMC_NOWAIT_HPP

#include <atomic>
#include <array>
#include <optional>
#include <thread>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>
#include <type_traits>
#include <variant>

namespace lf {

    // unary void template
    template <typename>
    using unary_void_t = void;

    template <template <typename> typename T, template <typename> typename U>
    inline constexpr bool is_same_template_v = false;
    template <template <typename> typename T>
    inline constexpr bool is_same_template_v<T, T> = true;

    // ---------- Hazard Pointer Infrastructure ----------
    // Fixed-size pool; tune for your worst-case concurrent thread count.
    inline constexpr std::size_t MAX_HAZARD_RECORD_COUNT = 128;

    // One hazard record
    struct Hazard_Record {
        std::atomic<std::thread::id> _owner_thread{};
        std::atomic<void*> _ptr{ nullptr };
    };

    // ---------- Retire & Reclaim (epoch-less HP scheme) ----------
    // Each retired node needs a deleter. The classic pattern uses an erased deleter pointer.
    struct Retired_Node {
        void* _ptr{};
        void(*_deleter)(void*){};
    };

    inline thread_local std::vector<Retired_Node> RETIRED_NODES;

    // Acquire/release one hazard record per thread via RAII.
    template <std::size_t HAZARD_RECORD_COUNT = MAX_HAZARD_RECORD_COUNT>
    class Hazard_Owner {
    public:
        inline constexpr std::size_t RECLAIM_TRESHOLD = 64;
        static inline Hazard_Record HAZARD_RECORDS[HAZARD_RECORD_COUNT];

        // Collect all *currently published* hazard pointers into a vector.
        static std::vector<void*> snapshot_hazards() {
            std::vector<void*> hazard_pointers;
            hazard_pointers.reserve(HAZARD_RECORD_COUNT);
            for (auto& hazard_record : HAZARD_RECORDS) {
                // Acquire to see a published hazard corresponding to a valid owner.
                if (hazard_record._owner_thread.load(std::memory_order_acquire) != std::thread::id{}) {
                    if (void* ptr = hazard_record._ptr.load(std::memory_order_acquire)) {
                        hazard_pointers.push_back(ptr);
                    }
                }
            }
            return hazard_pointers;
        }

        // Try to delete retired nodes that are not protected by any hazard pointer.
        static void try_reclaim() {
            if (RETIRED_NODES.empty()) return;

            auto hazard_ptrs = snapshot_hazards();
            std::vector<Retired_Node> retired_nodes__keep;
            retired_nodes__keep.reserve(RETIRED_NODES.size());
            for (auto& retired_node : RETIRED_NODES) {
                if (std::find(hazard_ptrs.begin(), hazard_ptrs.end(), retired_node._ptr) == hazard_ptrs.end()) {
                    retired_node._deleter(retired_node._ptr);
                } else {
                    retired_nodes__keep.push_back(retired_node);
                }
            }
            RETIRED_NODES.swap(retired_nodes__keep);
        }

        // Enqueue a node for later deletion; actually reclaim in batches.
        static inline void retire_later(void* ptr, void (*deleter)(void*)) {
            RETIRED_NODES.push_back(Retired_Node{ptr, deleter});
            if (RETIRED_NODES.size() >= RECLAIM_TRESHOLD) try_reclaim();
        }

        Hazard_Owner() : _hazard_record(acquire_record()) {}
        Hazard_Owner(Hazard_Owner&& rhs) noexcept : _hazard_record(rhs._hazard_record) {
            rhs._hazard_record = nullptr;
        }
        Hazard_Owner& operator=(Hazard_Owner&& rhs) noexcept {
            if (this != &rhs) {
                reset();
                _hazard_record = rhs._hazard_record;
                rhs._hazard_record = nullptr;
            }
            return *this;
        }
        Hazard_Owner(const Hazard_Owner&) = delete;
        Hazard_Owner& operator=(const Hazard_Owner&) = delete;

        ~Hazard_Owner() { reset(); }

        // Publish a protected hazard pointer.
        // notice memory order release.
        void protect(void* ptr) const noexcept {
            _hazard_record->_ptr.store(ptr, std::memory_order_release);
        }

        void* get() const noexcept {
            return _hazard_record ? _hazard_record->_ptr.load(std::memory_order_acquire) : nullptr;
        }

        // Clear hazard when done (before actually deleting the object).
        void clear() const noexcept {
            _hazard_record->_ptr.store(nullptr, std::memory_order_release);
        }

    private:
        Hazard_Record* _hazard_record{};

        static Hazard_Record* acquire_record() {
            auto tid = std::this_thread::get_id();
            for (auto& hazard_record : HAZARD_RECORDS) {
                std::thread::id empty{};
                if (
                    hazard_record._owner_thread.compare_exchange_strong(
                        empty,
                        tid,
                        std::memory_order_acq_rel))
                {
                    // Claimed this hazard record
                    return &hazard_record;
                }
                // If we already own one (re-entrant use in same thread), reuse it.
                if (hazard_record._owner_thread.load(std::memory_order_acquire) == tid) {
                    return &hazard_record;
                }
            }
            // Out of hazard slots: either increase HAZARD_RECORD_COUNT or use a dynamic registry.
            std::terminate();
        }

        void reset() {
            if (!_hazard_record) return;
            _hazard_record->_ptr.store(nullptr, std::memory_order_release);
            _hazard_record->_owner_thread.store(std::thread::id{}, std::memory_order_release);
            _hazard_record = nullptr;
        }
    };







    template <
        typename T,
        template <typename> typename Memory_Pool = unary_void_t,
        std::size_t HAZARD_RECORD_COUNT = MAX_HAZARD_RECORD_COUNT>
    class lock_free_stack {
    private:
        static constexpr bool _IS_MEMORY_POOL_VOID = is_same_template_v<Memory_Pool, unary_void_t>;

        struct Node {
            T _data;
            Node* _next{};
            explicit Node(T&& data) : _data(std::move(data)) {}
            explicit Node(const T& data) : _data(data) {}
        };

        using Memory_Pool_Type = std::conditional_t<
            _IS_MEMORY_POOL_VOID,
            std::monostate,
            Memory_Pool<Node>>;

        Memory_Pool_Type _memory_model;
        std::atomic<Node*> _head{nullptr};

        static void delete_node(void* ptr) {
            if constexpr (_IS_MEMORY_POOL_VOID) {
                delete static_cast<Node*>(ptr);
            } else {
                ptr->~Node();
                _memory_pool.release(static_cast<Node*>(ptr));
            }
        }

    public:

        lock_free_stack() requires _IS_MEMORY_POOL_VOID {};
        template <template <typename> typename M = Memory_Pool>
        explicit lock_free_stack(auto&& memory_pool) requires (!_IS_MEMORY_POOL_VOID)
            : _memory_model(std::forward<decltype(memory_pool)>(memory_pool)) {};

        ~lock_free_stack() {
            // Drain remaining nodes (single-threaded context assumed at destruction).
            Node* old_head = _head.load(std::memory_order_relaxed);
            if constexpr (_IS_MEMORY_POOL_VOID) {
                while (old_head) {
                    Node* next = old_head->_next;
                    delete old_head;
                    old_head = next;
                }
            } else {
                while (old_head) {
                    Node* next = old_head->_next;
                    old_head->~Node();
                    _memory_pool.release(old_head);
                    old_head = next;
                }
            }
            // Reclaim anything this thread still holds retired (best-effort).
            for (auto& retired_node : RETIRED_NODES) retired_node._deleter(retired_node._ptr);
            RETIRED_NODES.clear();
        }

        // Non-copyable / movable as you prefer; here: non-copyable, non-movable for simplicity.
        lock_free_stack(const lock_free_stack&) = delete;
        lock_free_stack& operator=(const lock_free_stack&) = delete;
        lock_free_stack(lock_free_stack&&) = delete;
        lock_free_stack& operator=(lock_free_stack&&) = delete;

        // push: classic CAS loop, publish with release
        void push(const T& data) {
            Node* new_head;
            if constexpr (_IS_MEMORY_POOL_VOID) {
                new_head = new Node(data);
            } else {
                new_head = _memory_pool.claim();
                new (new_head) Node(data);
            }
            new_head->_next = _head.load(std::memory_order_relaxed);
            while (
                !_head.compare_exchange_weak(
                    new_head->_next,
                    new_head,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {}
        }

        void push(T&& data) {
            Node* new_head;
            if constexpr (_IS_MEMORY_POOL_VOID) {
                new_head = new Node(std::move(data));
            } else {
                new_head = _memory_pool.claim();
                new (new_head) Node(std::move(data));
            }
            new_head->_next = _head.load(std::memory_order_relaxed);
            while (
                !_head.compare_exchange_weak(
                    new_head->_next,
                    new_head,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {}
        }

        // pop: protect head with a hazard pointer, then CAS it off
        // Returns std::optional<T>: empty if stack was empty.
        std::optional<T> pop() {
            Hazard_Owner<HAZARD_RECORD_COUNT> hazard_owner; // Acquire per-thread hazard record (RAII)
            while (true) {
                Node* old = _head.load(std::memory_order_acquire);
                if (!old) {
                    // Nothing to pop
                    return std::nullopt;
                }

                // Publish hazard = old (release), then recheck head to avoid races.
                hazard_owner.protect(old);

                // Make sure 'old' is still the head we saw (avoid torn protection).
                if (_head.load(std::memory_order_acquire) != old) {
                    continue; // Head changed; retry with new head and update hazard accordingly
                }

                Node* next = old->_next;
                if (_head.compare_exchange_weak(
                        old,
                        next,
                        std::memory_order_acquire,   // acquire pairs with push's release (reads old->_data safely)
                        std::memory_order_relaxed)) {
                    // We own 'old' now. Clear hazard before reclaiming so others can free it if safe.
                    hazard_owner.clear();

                    // Move out the payload under our exclusive ownership.
                    std::optional<T> out{std::move(old->_data)};

                    // Retire node; deleted when no hazard pointers point to it.
                    Hazard_Owner<HAZARD_RECORD_COUNT>::retire_later(
                        static_cast<void*>(old),
                        &delete_node);

                    // Opportunistic local reclaim if a batch built up.
                    Hazard_Owner<HAZARD_RECORD_COUNT>::try_reclaim();
                    return out;
                }
                // CAS failed: someone else popped it, retry with new head; loop will update hazard.
            }
        }

        bool empty() const noexcept {
            return _head.load(std::memory_order_acquire) == nullptr;
        }
    };

} // namespace lf

#endif // STACK_LF_LINKED_MPMC_NOWAIT_HPP
