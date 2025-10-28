#ifndef CONCURRENT_STACK_LF_LINKED_MPMC_HAZARD_HPP
#define CONCURRENT_STACK_LF_LINKED_MPMC_HAZARD_HPP

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
#include "Hazard_Ptr.hpp"

namespace BA_Concurrency {
    // use stack_LF_linked_MPMC_hazard alias
    // at the end of this file to get the defaults:
    //     Allocator = std::allocator<T> -> no memory pool (default new/delete)
    //     Hazard_Ptr_Record_Count = HAZARD_PTR_RECORD_COUNT__DEFAULT
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
        std::integral_constant<std::size_t, Hazard_Ptr_Record_Count>
    >
        : public Linked_Container<T, Allocator>
    {
        using traits = std::allocator_traits<Allocator>;
        using allocator_type = Allocator;

        allocator_type _allocator;
        std::atomic<Node*> _head{ nullptr };

        // deleter to be supplied to Hazard_Ptr_Owner for deferred reclamation
        static void delete_node(void* ptr) {
            traits::deallocate(_allocator, ptr, 1);
        }

    public:

        Stack() {};
        explicit Stack(auto&& allocator) : _allocator(std::forward<Allocator>(allocator)) {};

        ~Stack() {
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
        Stack(const Stack&) = delete;
        Stack& operator=(const Stack&) = delete;
        Stack(Stack&&) = delete;
        Stack& operator=(Stack&&) = delete;

        // push function with classic CAS loop
        template <typename U=T>
        void push(const U& data) {
            Node* new_head = traits::allocate(_allocator, 1);
            new_head->_next = _head.load(std::memory_order_relaxed);
            while (
                !_head.compare_exchange_weak(
                    new_head->_next,
                    new_head,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {}
        }

        // pop function:
        //   protect head with a hazard ptr and perform CAS
        std::optional<T> pop() {
            Hazard_Ptr_Owner<Hazard_Ptr_Record_Count> hazard_ptr_owner;
            Node* old_head = _head.load();
            do {
                Node* temp;
                hazard_ptr_owner.protect(old_head);
                do {
                    temp = old_head;
                    hazard_ptr_owner.protect(old_head); // protect by a hazard ptr
                    old_head = _head.load();
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
                Hazard_Ptr_Owner<Hazard_Ptr_Record_Count>::reclaim_memory_later(
                    static_cast<void*>(old_head),
                    &delete_node);

                // reclaim the nodes that are not protected by any hazard ptr
                Hazard_Ptr_Owner<Hazard_Ptr_Record_Count>::try_reclaim_memory();
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
    using stack_LF_linked_MPMC_hazard = Concurrent_Stack<
        true,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        T,
        Allocator,
        std::integral_constant<std::size_t, Hazard_Ptr_Record_Count>>;
} // namespace BA_Concurrency

#endif // CONCURRENT_STACK_LF_LINKED_MPMC_HAZARD_HPP
