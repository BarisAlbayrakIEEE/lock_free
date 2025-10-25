// lock_free_stack_hazard.hpp
#pragma once
#include <atomic>
#include <array>
#include <optional>
#include <thread>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>

namespace lf {

// ---------- Hazard Pointer Infrastructure ----------
// Fixed-size pool; tune for your worst-case concurrent thread count.
inline constexpr std::size_t kMaxHazardSlots = 128;

// One hazard record = (owner thread id, protected pointer)
struct HazardRecord {
    std::atomic<std::thread::id> owner{};
    std::atomic<void*>           ptr{nullptr};
};

inline HazardRecord g_hazard[kMaxHazardSlots];

// Acquire/release one hazard slot per thread via RAII.
class HazardOwner {
public:
    HazardOwner() : rec_(acquire_record_()) {}
    HazardOwner(HazardOwner&& other) noexcept : rec_(other.rec_) { other.rec_ = nullptr; }
    HazardOwner& operator=(HazardOwner&& other) noexcept {
        if (this != &other) {
            reset();
            rec_ = other.rec_;
            other.rec_ = nullptr;
        }
        return *this;
    }
    HazardOwner(const HazardOwner&) = delete;
    HazardOwner& operator=(const HazardOwner&) = delete;

    ~HazardOwner() { reset(); }

    // Publish a protected pointer (hazard). Use release to publish.
    void protect(void* p) const noexcept {
        rec_->ptr.store(p, std::memory_order_release);
    }

    void* get() const noexcept { return rec_ ? rec_->ptr.load(std::memory_order_acquire) : nullptr; }

    // Clear hazard when done (before actually deleting the object).
    void clear() const noexcept {
        rec_->ptr.store(nullptr, std::memory_order_release);
    }

private:
    HazardRecord* rec_{};

    static HazardRecord* acquire_record_() {
        auto me = std::this_thread::get_id();
        for (auto& h : g_hazard) {
            std::thread::id empty{};
            if (h.owner.compare_exchange_strong(empty, me, std::memory_order_acq_rel)) {
                // Claimed this slot
                return &h;
            }
            // If we already own one (re-entrant use in same thread), reuse it.
            if (h.owner.load(std::memory_order_acquire) == me) {
                return &h;
            }
        }
        // Out of hazard slots: either increase kMaxHazardSlots or use a dynamic registry.
        std::terminate();
    }

    void reset() {
        if (!rec_) return;
        rec_->ptr.store(nullptr, std::memory_order_release);
        rec_->owner.store(std::thread::id{}, std::memory_order_release);
        rec_ = nullptr;
    }
};

// Collect all *currently published* hazard pointers into a vector.
inline std::vector<void*> snapshot_hazards() {
    std::vector<void*> hp;
    hp.reserve(kMaxHazardSlots);
    for (auto& h : g_hazard) {
        // Acquire to see a published hazard corresponding to a valid owner.
        if (h.owner.load(std::memory_order_acquire) != std::thread::id{}) {
            if (void* p = h.ptr.load(std::memory_order_acquire)) {
                hp.push_back(p);
            }
        }
    }
    return hp;
}

// ---------- Retire & Reclaim (epoch-less HP scheme) ----------
// Each retired node needs a deleter. The classic pattern uses an erased deleter pointer.
struct RetiredNode {
    void*                 p{};
    void                (*deleter)(void*){};
};

inline thread_local std::vector<RetiredNode> t_retired;
inline constexpr std::size_t kReclaimThreshold = 64;

// Try to delete retired nodes that are not protected by any hazard pointer.
inline void try_reclaim() {
    if (t_retired.empty()) return;

    // 1) Snapshot hazards
    auto hazards = snapshot_hazards();

    // 2) Keep those that are still hazardous; delete the rest
    std::vector<RetiredNode> keep;
    keep.reserve(t_retired.size());

    for (auto& r : t_retired) {
        if (std::find(hazards.begin(), hazards.end(), r.p) == hazards.end()) {
            // Safe to delete
            r.deleter(r.p);
        } else {
            keep.push_back(r);
        }
    }
    t_retired.swap(keep);
}

// Enqueue a node for later deletion; actually reclaim in batches.
inline void retire_later(void* p, void (*deleter)(void*)) {
    t_retired.push_back(RetiredNode{p, deleter});
    if (t_retired.size() >= kReclaimThreshold) {
        try_reclaim();
    }
}

// ---------- Lock-Free Stack (LIFO) with Hazard Pointers ----------
template <typename T>
class lock_free_stack {
private:
    struct node {
        T     data;
        node* next;
        explicit node(T&& v) : data(std::move(v)), next(nullptr) {}
        explicit node(const T& v) : data(v), next(nullptr) {}
    };

    std::atomic<node*> head_{nullptr};

    static void delete_node(void* p) {
        delete static_cast<node*>(p);
    }

public:
    lock_free_stack() = default;
    ~lock_free_stack() {
        // Drain remaining nodes (single-threaded context assumed at destruction).
        node* n = head_.load(std::memory_order_relaxed);
        while (n) {
            node* next = n->next;
            delete n;
            n = next;
        }
        // Reclaim anything this thread still holds retired (best-effort).
        for (auto& r : t_retired) r.deleter(r.p);
        t_retired.clear();
    }

    // Non-copyable / movable as you prefer; here: non-copyable, non-movable for simplicity.
    lock_free_stack(const lock_free_stack&) = delete;
    lock_free_stack& operator=(const lock_free_stack&) = delete;
    lock_free_stack(lock_free_stack&&) = delete;
    lock_free_stack& operator=(lock_free_stack&&) = delete;

    // push: classic CAS loop, publish with release
    void push(const T& v) {
        node* n = new node(v);
        n->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(
            n->next, n,
            std::memory_order_release,    // success: publish n->data before new head
            std::memory_order_relaxed)) { // failure: just retry with updated expected
            /* retry */
        }
    }

    void push(T&& v) {
        node* n = new node(std::move(v));
        n->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(
            n->next, n,
            std::memory_order_release,
            std::memory_order_relaxed)) {
            /* retry */
        }
    }

    // pop: protect head with a hazard pointer, then CAS it off
    // Returns std::optional<T>: empty if stack was empty.
    std::optional<T> pop() {
        HazardOwner hp; // Acquire per-thread hazard slot (RAII)
        while (true) {
            node* old = head_.load(std::memory_order_acquire);
            if (!old) {
                // Nothing to pop
                return std::nullopt;
            }

            // Publish hazard = old (release), then recheck head to avoid races.
            hp.protect(old);

            // Make sure 'old' is still the head we saw (avoid torn protection).
            if (head_.load(std::memory_order_acquire) != old) {
                continue; // Head changed; retry with new head and update hazard accordingly
            }

            node* next = old->next;
            if (head_.compare_exchange_weak(
                    old, next,
                    std::memory_order_acquire,   // acquire pairs with push's release (reads old->data safely)
                    std::memory_order_relaxed)) {
                // We own 'old' now. Clear hazard before reclaiming so others can free it if safe.
                hp.clear();

                // Move out the payload under our exclusive ownership.
                std::optional<T> out{std::move(old->data)};

                // Retire node; deleted when no hazards point to it.
                retire_later(static_cast<void*>(old), &delete_node);

                // Opportunistic local reclaim if a batch built up.
                try_reclaim();
                return out;
            }
            // CAS failed: someone else popped it, retry with new head; loop will update hazard.
        }
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == nullptr;
    }
};

} // namespace lf



#include <memory>

template <class T>
struct node {
    node(const T& data) : data(std::make_shared<T>(data)), next(nullptr) {} 
    std::shared_ptr<T> data;
    node* next;
};

struct Foo{};
void push(const Foo& data)
{
    node<Foo>* const new_node=new node<Foo>(data);
    new_node->next=head.load();
    while(!head.compare_exchange_weak(new_node->next,new_node));
}

std::atomic<node<Foo>*> head;
std::shared_ptr<Foo> pop() {
    node<Foo>* old_head=head.load();
    while(
        old_head &&
        !head.compare_exchange_weak(old_head,old_head->next));
    return old_head ? old_head->data : std::shared_ptr<Foo>();
}

int i = 5;
std::atomic<void*> get_hazard_pointer_for_current_thread() { int* ptr = &i; return ptr; };

std::shared_ptr<Foo> pop() {
    auto ptr = get_hazard_pointer_for_current_thread();
    std::atomic<void*>& hp = ptr;
    node<Foo>* old_head = head.load();
    node<Foo>* temp;
    do {
        temp = old_head;
        hp.store(old_head);
        old_head = head.load();
        } while(old_head != temp);
}
