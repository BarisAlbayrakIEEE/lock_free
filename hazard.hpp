// lock_free_stack_hazard.hpp

#ifndef STACK_LF_LINKED_MPMC_NOWAIT_HPP
#define STACK_LF_LINKED_MPMC_NOWAIT_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <type_traits>
#include <utility>
#include <new>
#include <optional>
#include <variant>
#include <thread>
#include <vector>
#include <algorithm>

namespace lf {
    inline constexpr std::size_t HAZARD_RECORD_COUNT__DEFAULT = 128;

    template <template <typename> typename T, template <typename> typename U>
    inline constexpr bool is_same_template_v = false;
    template <template <typename> typename T>
    inline constexpr bool is_same_template_v<T, T> = true;

    template <typename>
    using unary_void_t = void;

    // hazard pointer class definition
    struct Hazard_Record {
        std::atomic<std::thread::id> _owner_thread{};
        std::atomic<void*> _ptr{ nullptr };
    };

    // a wrapper for the retired objects to perform deferred reclamation
    struct Retired_Object {
        void* _ptr{};
        void(*_deleter)(void*){};
    };
    inline thread_local std::vector<Retired_Object> RETIRED_OBJECTS;

    // RAII class for the hazard pointers
    template <std::size_t HAZARD_RECORD_COUNT = HAZARD_RECORD_COUNT__DEFAULT>
    class Hazard_Owner {
    public:
        inline constexpr std::size_t RECLAIM_TRESHOLD = 64;
        static inline Hazard_Record HAZARD_RECORDS[HAZARD_RECORD_COUNT];

        // collect all currently published hazard pointers into a vector
        static std::vector<void*> snapshot_hazards() {
            std::vector<void*> hazard_pointers;
            hazard_pointers.reserve(HAZARD_RECORD_COUNT);
            std::thread::id empty_tid{};
            for (auto& hazard_record : HAZARD_RECORDS) {
                if (hazard_record._owner_thread.load(std::memory_order_acquire) != empty_tid) {
                    if (void* ptr = hazard_record._ptr.load(std::memory_order_acquire)) {
                        hazard_pointers.push_back(ptr);
                    }
                }
            }
            return hazard_pointers;
        }

        // try to delete retired objects that are not protected by any hazard pointer
        static void try_reclaim() {
            if (RETIRED_OBJECTS.empty()) return;

            auto hazard_ptrs = snapshot_hazards();
            std::vector<Retired_Object> retired_objects__keep;
            retired_objects__keep.reserve(RETIRED_OBJECTS.size());
            for (auto& retired_object : RETIRED_OBJECTS) {
                if (
                    std::find(
                        hazard_ptrs.begin(),
                        hazard_ptrs.end(),
                        retired_object._ptr) ==
                    hazard_ptrs.end())
                {
                    retired_object._deleter(retired_object._ptr);
                } else {
                    retired_objects__keep.push_back(retired_object);
                }
            }
            RETIRED_OBJECTS.swap(retired_objects__keep);
        }

        // add the input object into the retired object list for later reclamation
        static inline void retire_later(void* ptr, void (*deleter)(void*)) {
            RETIRED_OBJECTS.push_back(Retired_Object{ptr, deleter});
            if (RETIRED_OBJECTS.size() >= RECLAIM_TRESHOLD) try_reclaim();
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

        // publish a protected hazard pointer
        void protect(void* ptr) const noexcept {
            _hazard_record->_ptr.store(ptr, std::memory_order_release);
        }

        void* get() const noexcept {
            return
                _hazard_record ?
                _hazard_record->_ptr.load(std::memory_order_acquire) :
                nullptr;
        }

        // clear hazard when the protection is no longer needed
        void clear() const noexcept {
            _hazard_record->_ptr.store(nullptr, std::memory_order_release);
        }

    private:
        Hazard_Record* _hazard_record{};

        static Hazard_Record* acquire_record() {
            auto this_tid = std::this_thread::get_id();
            for (auto& hazard_record : HAZARD_RECORDS) {
                std::thread::id old_tid{};
                if (
                    hazard_record._owner_thread.compare_exchange_strong(
                        old_tid,
                        this_tid,
                        std::memory_order_acq_rel))
                    return &hazard_record;

                // if already owned (re-entrant use in same thread)
                if (hazard_record._owner_thread.load(std::memory_order_acquire) == this_tid)
                    return &hazard_record;
            }
            // TODO: Out of hazard slots: either increase HAZARD_RECORD_COUNT or use a dynamic registry.
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
        std::size_t HAZARD_RECORD_COUNT = HAZARD_RECORD_COUNT__DEFAULT>
        requires ( // for the thread safety of pop as it returns std::optional<T>
            std::is_nothrow_move_constructible_v<T> &&
            std::is_nothrow_move_assignable_v<T>)
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

        // deleter to be supplied to Hazard_Owner for deferred reclamation
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
            // delete the not-yet-reclaimed nodes if exists any
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

            // reclaim the retired nodes owned by this thread
            // delete the retired objects manually for a better performance
            // than try_reclaim function which is too noisy in case of this destructor
            for (auto& retired_object : RETIRED_OBJECTS)
                retired_object._deleter(retired_object._ptr);
            RETIRED_OBJECTS.clear();
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
            Hazard_Owner<HAZARD_RECORD_COUNT> hazard_owner; // per thread hazard record (RAII)
            Node* old_head = _head.load();
            do {
                Node* temp;
                hazard_owner.protect(old_head);
                do {
                    temp = old_head;
                    hazard_owner.protect(old_head); // protect by a hazard pointer
                    old_head = _head.load();
                } while(old_head != temp);
            } while(
                old_head &&
                !_head.compare_exchange_strong( // set head to next taking the spurious failures into account
                    old_head,
                    old_head->_next,
                    std::memory_order_acquire,
                    std::memory_order_relaxed));
            
            // clear the hazard pointer
            hazard_owner.clear();

            std::optional<T> data{ std::nullopt };
            if (old_head) {
                // guaranteed to be safe as:
                //   std::is_nothrow_move_constructible_v<T> &&
                //   std::is_nothrow_move_assignable_v<T>
                std::optional<T> data{ std::move(old_head->_data) };

                // destroy the old head when all hazard pointers are cleared
                Hazard_Owner<HAZARD_RECORD_COUNT>::retire_later(
                    static_cast<void*>(old_head),
                    &delete_node);

                // reclaim the retired nodes that are not protected by any hazard pointer
                Hazard_Owner<HAZARD_RECORD_COUNT>::try_reclaim();
            }
            return data;







            /*
            TODO: SIL
            Hazard_Owner<HAZARD_RECORD_COUNT> hazard_owner; // Acquire per-thread hazard record (RAII)
            while (true) {
                Node* old_head = _head.load(std::memory_order_acquire);
                if (!old_head) return std::nullopt;

                // protext the old head by a hazard pointer
                hazard_owner.protect(old_head);

                // check if the head is updated by another thread
                if (_head.load(std::memory_order_acquire) != old_head) {
                    hazard_owner.clear();
                    continue;
                }

                // set the new head
                if (_head.compare_exchange_weak(
                        old_head,
                        old_head->_next,
                        std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    // clear the hazard pointer
                    hazard_owner.clear();

                    // guaranteed to be safe as:
                    //   std::is_nothrow_move_constructible_v<T> &&
                    //   std::is_nothrow_move_assignable_v<T>
                    std::optional<T> data{ std::move(old_head->_data) };

                    // destroy the old head when all hazard pointers are cleared
                    Hazard_Owner<HAZARD_RECORD_COUNT>::retire_later(
                        static_cast<void*>(old_head),
                        &delete_node);

                    // Opportunistic local reclaim if a batch built up.
                    Hazard_Owner<HAZARD_RECORD_COUNT>::try_reclaim();
                    return data;
                }
            }
            */
        }

        bool empty() const noexcept {
            return _head.load(std::memory_order_acquire) == nullptr;
        }
    };

} // namespace lf

























// RingStack: bounded lock-free MPMC stack over a contiguous ring buffer.
// Requirements:
// - _CAPACITY must be a power of two (for fast masking).
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

template <typename T, unsigned char Capacity_As_Power_of_Two>
requires (
    std::is_nothrow_constructible_v<T> && // for emplace
    std::is_nothrow_move_constructible_v<T> && // for push and pop
    std::is_nothrow_move_assignable_v<T>) // for pop
class RingStack {
    struct Slot {
        std::atomic<std::uint64_t> _ticket_index; // will be initialized during the stack's constructor
        alignas(T) unsigned char _storage[sizeof(T)];
        T* to_ptr() noexcept { return std::launder(reinterpret_cast<T*>(_storage)); }
    };

    // capacity of the ring buffer is allowed to be powers of two only
    static constexpr std::size_t _CAPACITY = std::size_t(1) << Capacity_As_Power_of_Two;
    // mask to modulo the ticket by _CAPACITY
    static constexpr std::size_t _MASK = _CAPACITY - 1;
    // _top is the next ticket to push.
    std::atomic<std::uint64_t> _top{0};
    // the buffer of slots
    Slot _slots[_CAPACITY];

    template <class U>
    bool push_nonblocking(U&& data) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
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
    RingStack() noexcept {
        for (std::uint64_t i = 0; i < _CAPACITY; ++i) {
            _slots[i]._ticket_index.store(i, std::memory_order_relaxed);
        }
    }

    // Assume single-threaded destruction.
    // Remove the remaining (not-yet-popped) elements
    // by inspecting if the buffer element at the buffer index is full.
    // Because _top is monotonic and the destrructor is assumed single-threaded,
    // it is safe to just attempt destruction when the slot is full.
    //     empty: buffer index % _CAPACITY == i
    //     full : buffer index % _CAPACITY == (i + 1) % _CAPACITY
    // No need to reset the buffer index sequence here in the destructor.
    ~RingStack() {
        for (std::uint64_t i = 0; i < _CAPACITY; ++i) {
            auto ticket_index = _slots[i]._ticket_index.load(std::memory_order_relaxed);
            if (((ticket_index - 1) & _MASK) == i) _slots[i].to_ptr()->~T();
        }
    }

    // Non-copyable / non-movable for simplicity
    RingStack(const RingStack&) = delete;
    RingStack& operator=(const RingStack&) = delete;

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
                slot._ticket_index.store(ticket + _CAPACITY, std::memory_order_release);
                return data;
            }
            // CAS failed: retry CAS with the updated expected value
        }
    }
};

#endif // STACK_LF_LINKED_MPMC_NOWAIT_HPP
