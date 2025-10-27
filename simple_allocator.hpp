#ifndef SIMPLE_ALLOCATOR_HPP
#define SIMPLE_ALLOCATOR_HPP

#include <cstddef>
#include "simple_static_arena.hpp"

namespace BA_Concurrency {
    template <
        class T,
        size_t N,
        typename Alignment,
        typename Arena_Type = SSA_t<N>>
    struct simple_allocator {
    public:
        using value_type = T;
        using arena_type = Arena_Type;

        simple_allocator(const simple_allocator&) = default;
        simple_allocator& operator=(const simple_allocator&) = default;
        simple_allocator(arena_type& arena) noexcept : _arena{&arena} {};
        template <class U>
        simple_allocator(const simple_allocator<U, N, Alignment, Arena_Type>& other) noexcept : _arena{other._arena} {}
        
        template <class U>
        struct rebind {
            using other = simple_allocator<U, N, Alignment, Arena_Type>;
        };
        auto allocate(size_t n) -> T* {
            return reinterpret_cast<T*>(_arena->allocate(n*sizeof(T)));
        }
        auto deallocate(T* p, size_t n) noexcept -> void {
            _arena->deallocate(reinterpret_cast<std::byte*>(p), n*sizeof(T));
        }
        template <class U, size_t M>
        auto operator==(const simple_allocator<U, M, Alignment, Arena_Type>& other) const noexcept {
            return N == M && _arena == other._arena;
        }
        template <class U, size_t M>
        auto operator!=(const simple_allocator<U, M, Alignment, Arena_Type>& other) const noexcept {
            return !(*this == other);
        }
        template <class U, size_t M, typename Alignment2, typename Arena_Type2> friend struct simple_allocator;
    private:
        arena_type* _arena;
    };

    // alias for the default
    template <class T, size_t N>
    using SA_t = simple_allocator<T, N, SSA_t<N>>;
}

#endif // SIMPLE_ALLOCATOR_HPP