#ifndef SIMPLE_ALLOCATOR_HPP
#define SIMPLE_ALLOCATOR_HPP

#include <cstddef>
#include <type_traits>
#include "Simple_Static_Arena.hpp"

namespace BA_Concurrency {
    template <
        class T,
        size_t N,
        size_t Alignment,
        typename Arena = Simple_Static_Arena<N, Alignment>>
    class Simple_Allocator {
    public:
        using value_type = T;
        using arena_type = Arena;

        Simple_Allocator(const Simple_Allocator&) = default;
        Simple_Allocator& operator=(const Simple_Allocator&) = default;
        Simple_Allocator(arena_type& arena) noexcept : _arena{&arena} {};
        template <class U>
        Simple_Allocator(const Simple_Allocator<U, N, Alignment, arena_type>& other) noexcept
            : _arena{ other._arena } {}
        
        template <class U>
        struct rebind {
            using other = Simple_Allocator<U, N, Alignment, arena_type>;
        };
        auto allocate(size_t n) -> T* {
            return reinterpret_cast<T*>(_arena->allocate(n*sizeof(T)));
        }
        auto deallocate(T* p, size_t n) noexcept -> void {
            _arena->deallocate(reinterpret_cast<std::byte*>(p), n*sizeof(T));
        }
        template <class U, size_t M>
        auto operator==(const Simple_Allocator<U, M, Alignment, arena_type>& other) const noexcept {
            return N == M && _arena == other._arena;
        }
        template <class U, size_t M>
        auto operator!=(const Simple_Allocator<U, M, Alignment, arena_type>& other) const noexcept {
            return !(*this == other);
        }
        template <class U, size_t M, typename Alignment2, typename Arena_Type2> friend class Simple_Allocator;
    private:
        arena_type* _arena;
    };

    // alias for the default
    template <class T, size_t N>
    using SA_t = Simple_Allocator<T, N, alignof(T), Simple_Static_Arena<N, alignof(T)>>;
} // namespace BA_Concurrency

#endif // SIMPLE_ALLOCATOR_HPP
