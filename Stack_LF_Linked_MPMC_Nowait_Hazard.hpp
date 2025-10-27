#ifndef STACK_LF_LINKED_MPMC_NOWAIT_HAZARD_HPP
#define STACK_LF_LINKED_MPMC_NOWAIT_HAZARD_HPP

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
#include "Linked_Container.hpp"
#include "Stack_Base.hpp"
#include "Hazard_Ptr.hpp"

namespace BA_Concurrency {
    // use stack_LF_linked_MPMC_nowait__hazard alias
    // at the end of this file to get the defaults:
    //     Allocator = unary_void_t -> no memory pool (default new/delete)
    //     Hazard_Ptr_Record_Count = HAZARD_PTR_RECORD_COUNT__DEFAULT
    template <
        typename T,
        typename Allocator,
        std::size_t Hazard_Ptr_Record_Count>
    requires ( // for the thread safety of pop as it returns std::optional<T>
            std::is_nothrow_move_constructible_v<T> &&
            std::is_nothrow_move_assignable_v<T>)
    class Stack<
        true,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        false,
        T,
        Allocator,
        std::integral_constant<std::size_t, Hazard_Ptr_Record_Count>
    >
        : public Linked_Container<T, Allocator>
    {
        using traits = std::allocator_traits<Allocator>;
        using allocator_type = Allocator;
        allocator_type _allocator;

        Node* allocate_node() {
            return traits::allocate(_allocator, 1);
        }

        void deallocate_node(Node* p) {
            traits::deallocate(_allocator, ptr, 1);
        }







        typename Base::allocator_type _allocator;
        std::atomic<Node*> _head{nullptr};

        // deleter to be supplied to Hazard_Ptr_Owner for deferred reclamation
        static void delete_node(void* ptr) {
            traits::deallocate(_allocator, ptr, 1);
        }

    public:

        Stack() requires IAV_v {};
        template <template <typename> typename M = Allocator>
        explicit Stack(auto&& allocator) requires (!IAV_v)
            : _allocator(std::forward<decltype(allocator)>(allocator)) {};

        ~Stack() {
            // delete the not-yet-reclaimed nodes if exists any
            Node* old_head = _head.load(std::memory_order_relaxed);
            if constexpr (IAV_v) {
                while (old_head) {
                    Node* next = old_head->_next;
                    delete old_head;
                    old_head = next;
                }
            } else {
                while (old_head) {
                    Node* next = old_head->_next;
                    _allocator.deallocate(old_head);
                    old_head = next;
                }
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
            Node* new_head;
            if constexpr (IAV_v) {
                new_head = new Node(std::forward<U>(data));
            } else {
                new_head = _allocator.allocate();
            }
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
                    std::memory_order_acquire,
                    std::memory_order_relaxed));
            
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
        template <typename> typename Allocator = unary_void_t,
        std::size_t Hazard_Ptr_Record_Count = HAZARD_PTR_RECORD_COUNT__DEFAULT>
    using stack_LF_linked_MPMC_nowait__hazard = Stack<
        true,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        false,
        T,
        Allocator,
        std::integral_constant<std::size_t, Hazard_Ptr_Record_Count>
    >;
} // namespace BA_Concurrency

#endif // STACK_LF_LINKED_MPMC_NOWAIT_HAZARD_HPP
