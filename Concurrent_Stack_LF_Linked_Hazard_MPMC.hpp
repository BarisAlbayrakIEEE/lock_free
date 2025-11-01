// Concurrent_Stack_LF_Linked_Hazard_MPMC.hpp
//
// Description:
//   The solution for the lock-free/linked/MPMC stack problem with hazard pointers:
//     Pop operation needs to reclaim the memory for the head node.
//     However, the other consumer threads working on the same head
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

#ifndef CONCURRENT_STACK_LF_LINKED_HAZARD_MPMC_HPP
#define CONCURRENT_STACK_LF_LINKED_HAZARD_MPMC_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <thread>
#include <vector>
#include <optional>
#include <algorithm>
#include <utility>
#include <type_traits>
#include <memory>
#include "Node.hpp"
#include "Concurrent_Stack.hpp"
#include "enum_memory_reclaimers.hpp"
#include "Hazard_Ptr.hpp"

namespace BA_Concurrency {
    // use stack_LF_linked_hazard_MPMC alias at the end of this file
    // to get the right specialization of Concurrent_Stack
    // and to achieve the default arguments consistently.
    template <
        typename T,
        typename Allocator,
        std::size_t Hazard_Ptr_Record_Count>
    requires ( // for the thread safety of pop as it returns std::optional<T>
            std::is_nothrow_move_constructible_v<T> &&
            std::is_nothrow_move_assignable_v<T>)
    class Concurrent_Stack<
        true,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        T,
        Allocator,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Memory_Reclaimers::Hazard_Ptr)>,
        std::integral_constant<std::size_t, Hazard_Ptr_Record_Count>>
    {
        using traits = std::allocator_traits<Allocator>;
        using allocator_type = Allocator;

        // local aliases
        using _HPO = Hazard_Ptr_Owner<Hazard_Ptr_Record_Count>;

        allocator_type _allocator;
        std::atomic<Node*> _head{ nullptr };

        // deleter to be supplied to Hazard_Ptr_Owner for deferred reclamation
        static void delete_node(void* ptr) {
            traits::deallocate(_allocator, ptr, 1);
        }

    public:

        Concurrent_Stack() {};
        explicit Concurrent_Stack(auto&& allocator) : _allocator(std::forward<Allocator>(allocator)) {};

        ~Concurrent_Stack() {
            // delete the not-yet-reclaimed nodes if exists any
            Node* old_head = _head.load(std::memory_order_relaxed);
            while (old_head) {
                Node* next = old_head->_next;
                traits::deallocate(_allocator, old_head, 1);
                old_head = next;
            }

            // reclaim all memory
            for (auto& memory_reclaimer : MEMORY_RECLAIMERS)
                memory_reclaimer._deleter(memory_reclaimer._ptr);
            MEMORY_RECLAIMERS.clear();
        }

        // Non-copyable/movable for simplicity.
        Concurrent_Stack(const Concurrent_Stack&) = delete;
        Concurrent_Stack& operator=(const Concurrent_Stack&) = delete;
        Concurrent_Stack(Concurrent_Stack&&) = delete;
        Concurrent_Stack& operator=(Concurrent_Stack&&) = delete;

        // push function with classic CAS loop
        template <typename U = T>
        void push(U&& data) {
            Node* new_head = traits::allocate(_allocator, 1);
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
        //   CAS the head to the next while being protected by a hazard ptr,
        //   clear the hazard ptr,
        //   extract data from the popped head,
        //   add the popped head to the reclaim list,
        //   return the data.
        std::optional<T> pop() {
            // CAS the head to the next while being protected by a hazard ptr
            _HPO hazard_ptr_owner;
            Node* old_head = _head.load(std::memory_order_acquire);
            do {
                Node* temp;
                do {
                    temp = old_head;
                    hazard_ptr_owner.protect(old_head); // protect by a hazard ptr
                    old_head = _head.load(std::memory_order_acquire);
                } while(old_head != temp);
            } while(
                old_head &&
                !_head.compare_exchange_strong( // set head to next taking the spurious failures into account
                    old_head,
                    old_head->_next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire));
            
            // clear the hazard ptr
            hazard_ptr_owner.clear();

            // return the data if popped successfully
            std::optional<T> data{ std::nullopt };
            if (old_head) {
                // guaranteed to be safe as:
                //   std::is_nothrow_move_constructible_v<T> &&
                //   std::is_nothrow_move_assignable_v<T>
                std::optional<T> data{ std::move(old_head->_data) };

                // destroy the old head when all hazard ptrs are cleared
                _HPO::reclaim_memory_later(static_cast<void*>(old_head), &delete_node);
            }
            return data;
        }

        bool empty() const noexcept {
            return _head.load(std::memory_order_acquire) == nullptr;
        }
    };

    template <
        typename T,
        typename Allocator = std::allocator<T>,
        std::size_t Hazard_Ptr_Record_Count = HAZARD_PTR_RECORD_COUNT__DEFAULT>
    using stack_LF_linked_hazard_MPMC = Concurrent_Stack<
        true,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        T,
        Allocator,
        std::integral_constant<std::uint8_t, static_cast<std::uint8_t>(Enum_Memory_Reclaimers::Hazard_Ptr)>,
        std::integral_constant<std::size_t, Hazard_Ptr_Record_Count>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_LF_LINKED_HAZARD_MPMC_HPP
