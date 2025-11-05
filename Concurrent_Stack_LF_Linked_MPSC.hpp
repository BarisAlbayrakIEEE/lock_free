// Concurrent_Stack_LF_Linked_MPSC.hpp
//
// Description:
//   The solution for the lock-free/linked/MPSC stack problem:
//     Single consumer terminates the need for the synchronization
//     during the head node destruction.
//     In other words, in case of a single consumer,
//     the hazard pointers (or reference counters) are not required.
//
// Requirements:
// - T must be noexcept-movable.
//
// Semantics:
//   push():
//     The classical push routine is applied:
//       1. Create a new node.
//       2. Set the next pointer of the new node to the current head.
//       3. Apply CAS on the head: CAS(new_node->head, new_node)
//   pop():
//     The classical pop routine is applied:
//       1. Apply CAS on the head: CAS(head, head->next)
//       2. Move the data out from the old head node
//       3. Delete the old head
//       4. Return the data
//
// Progress:
//   Lock-free:
//     Lock-free execution as the threads serializing on the head node
//     are bound to functions (push and pop) with constant time complexity, O(1).
//
// Cautions:
//   1. In case of a single producer (i.e. SPMC and SPSC),
//      the competition between the single producer and the consumer(s) remain
//      which means that the synchronization between the counterparts is still required.
//      On the other hand, single consumer configuration is special
//      and is explained in the 2nd caution of the main documentation of:
//        Concurrent_Stack_LF_Linked_Hazard_MPMC.hpp.
//      As the single producer configuration has no effect on this design,
//      I will use the same specialization for the following two configurations:
//        MPSC and SPMC
//      
//      See the two aliases at the end:
//        stack_LF_linked_MPSC
//        stack_LF_linked_SPSC = stack_LF_linked_MPSC
//   2. use stack_LF_Linked_MPSC and stack_LF_Linked_SPSC aliases at the end of this file
//      to get the right specialization of Concurrent_Stack
//      and to achieve the default arguments consistently.
//
// TODOs:
//   1. Consider exponential backoff for the head node
//      in order to deal with the high CAS contention on the head.

#ifndef CONCURRENT_STACK_LF_LINKED_MPSC_HPP
#define CONCURRENT_STACK_LF_LINKED_MPSC_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <optional>
#include <algorithm>
#include <utility>
#include <memory>
#include "Node.hpp"
#include "Concurrent_Stack.hpp"
#include "enum_memory_reclaimers.hpp"

namespace BA_Concurrency {
    // use stack_LF_linked_MPSC alias at the end of this file
    // to get the right specialization of Concurrent_Stack
    // and to achieve the default arguments consistently.
    template <
        typename T,
        template <typename> typename Allocator>
    requires ( // for the thread safety of pop as it returns std::optional<T>
            std::is_nothrow_move_constructible_v<T> &&
            std::is_nothrow_move_assignable_v<T>)
    class Concurrent_Stack<
        true,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPSC,
        T,
        Allocator<Node<T>>>
    {
        using allocator_type = Allocator<Node<T>>;
        using traits = std::allocator_traits<allocator_type>;

        allocator_type _allocator;
        std::atomic<Node<T>*> _head{ nullptr };

    public:

        Concurrent_Stack() = default;
        explicit Concurrent_Stack(auto&& allocator)
            : _allocator(std::forward<allocator_type>(allocator)) {};

        ~Concurrent_Stack() {
            // delete the not-yet-reclaimed nodes if exists any
            Node<T>* old_head = _head.load(std::memory_order_relaxed);
            while (old_head) {
                Node<T>* next = old_head->_next;
                traits::destroy(_allocator, old_head);
                traits::deallocate(_allocator, old_head, 1);
                old_head = next;
            }
        }

        // Non-copyable/movable for simplicity
        Concurrent_Stack(const Concurrent_Stack&) = delete;
        Concurrent_Stack& operator=(const Concurrent_Stack&) = delete;
        Concurrent_Stack(Concurrent_Stack&&) = delete;
        Concurrent_Stack& operator=(Concurrent_Stack&&) = delete;

        // push function with classic CAS loop
        //   1. Create a new node.
        //   2. Set the next pointer of the new node to the current head.
        //   3. Apply CAS on the head: CAS(new_node->head, new_node)
        template <typename U = T>
        void push(U&& data) {
            Node<T>* new_head = traits::allocate(_allocator, 1);
            new_head->_next = _head.load(std::memory_order_relaxed); // CAS loop will correct the next pointer
            new_head->_data = std::forward<U>(data);
            while (
                !_head.compare_exchange_weak(
                    new_head->_next,
                    new_head,
                    std::memory_order_release,
                    std::memory_order_relaxed));
        }

        // pop function:
        //   1. Apply CAS on the head: CAS(head, head->next)
        //   2. Move the data out from the old head node
        //   3. Delete the old head
        //   4. Return the data
        std::optional<T> pop() {
            Node<T>* old_head = _head.load(std::memory_order_acquire);

            // Apply CAS on the head: CAS(head, head->next)
            while(
                old_head &&
                !_head.compare_exchange_weak(
                    old_head,
                    old_head->_next,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed));
            if (!old_head) return std::nullopt;

            // extract the data
            std::optional<T> data{ std::move(old_head->_data) };
            
            // Delete the old head
            traits::destroy(_allocator, old_head);
            traits::deallocate(_allocator, old_head, 1);

            // return the data
            return data;
        }

        bool empty() const noexcept {
            return _head.load(std::memory_order_acquire) == nullptr;
        }
    };

    template <
        typename T,
        template <typename> typename Allocator = std::allocator>
    using stack_LF_linked_MPSC = Concurrent_Stack<
        true,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPSC,
        T,
        Allocator<Node<T>>>;

    // As explained in Caution 1 of the header documentation
    // SPSC configuration is same as MPSC
    template <
        typename T,
        template <typename> typename Allocator = std::allocator>
    using stack_LF_linked_SPSC = stack_LF_linked_MPSC<
        T,
        Allocator>;
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_LF_LINKED_MPSC_HPP
