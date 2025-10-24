#ifndef QUEUE_LF_LINKED_SPSC_NOWAIT_HPP
#define QUEUE_LF_LINKED_SPSC_NOWAIT_HPP

#include <cstddef>
#include <array>
#include <atomic>
#include <optional>
#include "queue_LF_linked_node.hpp"

template <class T, std::size_t N>
    requires std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_assignable_v<T> &&
             std::is_default_constructible_v<T>
class queue_LF_linked_SPSC_nowait {
    using _node_t = queue_LF_linked_node<T>;
    std::atomic<_node_t*> _head;

public:

    void push(T const& _data){
        auto new_node = std::make_shared<_Node_t>(_data);
        auto new_node_ptr = new_node.get();
        new_node_ptr->_next = _head.load();
        while(
            !_head.compare_exchange_weak(
                new_node_ptr->_next,
                new_node_ptr,
                std::memory_order_release,
                std::memory_order_relaxed));
    }

    std::shared_ptr<T> pop() {
        /*
            Read the current value of head.
            Read head->next.
            Set head to head->next.
            Return the data from the retrieved node.
            Delete the retrieved node.
        */
        _node_t* old_head = _head.load();
        while(
            old_head &&
            !_head.compare_exchange_weak(
                old_head,
                old_head->_next,
                std::memory_order_acquire,
                std::memory_order_relaxed));
        return old_head ? old_head->_data : std::shared_ptr<T>();
    }





    // strong exception safety
    bool push(T&& t) { return push_helper(std::move(t)); }
    // strong exception safety
    bool push(const T& t) { return push_helper(t); }
    
    // strong exception safety
    template<typename... Ts>
    bool emplace(Ts&&... args) {
        if (_size.load(std::memory_order_acquire) == N) return false;

        // can fail if T's ctor can throw
        // would be optimized by the compiler
        _static_buffer[_index__push] = T(std::forward<Ts>(args)...);
        _index__push = (_index__push + 1) % N; // no throw
        _size.fetch_add(1, std::memory_order_release); // no throw
        return true; // no throw
    }

    // strong exception safety
    auto pop() -> std::optional<T> {
        std::optional<T> val{};
        if (_size.load(std::memory_order_acquire) > 0) {
            val = std::move(_static_buffer[_index__pop]); // relies on std::is_nothrow_move_assignable_v<T>
            _index__pop = (_index__pop + 1) % N; // no throw
            _size.fetch_sub(1, std::memory_order_release); // no throw
        }
        return val; // no throw by std::is_nothrow_move_constructible_v<T>
    }

    // not reliable but not needed to be -> memory_order_relaxed
    auto size() const noexcept { return _size.load(std::memory_order_relaxed); }
};

#endif // QUEUE_LF_LINKED_SPSC_NOWAIT_HPP
