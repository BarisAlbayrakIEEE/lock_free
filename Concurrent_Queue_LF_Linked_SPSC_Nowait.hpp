#ifndef QUEUE_LF_LINKED_SPSC_NOWAIT_HPP
#define QUEUE_LF_LINKED_SPSC_NOWAIT_HPP

#include <cstddef>
#include <atomic>
#include <optional>
#include "queue_LF_linked_node.hpp"

template <class T>
    requires std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_assignable_v<T> &&
             std::is_default_constructible_v<T>
class queue_LF_linked_SPSC_nowait {
    using _node_t = queue_LF_linked_node<T>;
    std::atomic<_node_t*> _head;

public:

    void push(T const& data) {
        auto new_node = new _node_t(data);
        new_node->_next = _head.load();
        while(
            !_head.compare_exchange_weak(
                new_node->_next,
                new_node,
                std::memory_order_release,
                std::memory_order_relaxed));
    }

    std::optional<T> pop() {
        // Read the current value of head.
        // Read head->next.
        // Set head to head->next.
        // Return the data from the retrieved node.
        // Delete the retrieved node.
        std::optional<T> data{};
        _node_t* old_head = _head.load();
        while(
            old_head &&
            !_head.compare_exchange_weak(
                old_head,
                old_head->_next,
                std::memory_order_acquire,
                std::memory_order_relaxed));
        if (old_head) data = old_head->_data;
        return old_head ? data : std::nullopt;
    }
};

#endif // QUEUE_LF_LINKED_SPSC_NOWAIT_HPP
