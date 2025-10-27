#ifndef SIMPLE_STATIC_ARENA_HPP
#define SIMPLE_STATIC_ARENA_HPP

#include <cstddef>
#include <cstdint>

namespace BA_Concurrency {
    template <size_t N, typename Alignment = std::max_align_t>
    class simple_static_arena {
        static constexpr size_t alignment = alignof(Alignment);
        static auto align_up(size_t n) noexcept -> size_t {
            return (n + (alignment-1)) & ~(alignment-1);
        }
        auto pointer_in_buffer(const std::byte* p) const noexcept -> bool {
            return
                uintptr_t(p) >= uintptr_t(_buffer) &&
                uintptr_t(p) < uintptr_t(_buffer) + N;
        }
        alignas(alignment) std::byte _buffer[N];
        std::byte* _ptr{};

    public:

        simple_static_arena() noexcept : _ptr(_buffer) {}
        simple_static_arena(const simple_static_arena&) = delete;
        simple_static_arena& operator=(const simple_static_arena&) = delete;
        auto reset() noexcept { _ptr = _buffer; }
        static constexpr auto size() noexcept { return N; }
        auto used() const noexcept { return static_cast<size_t>(_ptr - _buffer); }
        auto allocate(size_t n) -> std::byte* {
            const auto aligned_n = align_up(n);
            const auto available_bytes = static_cast<decltype(aligned_n)>(_buffer + N - _ptr);
            if (available_bytes >= aligned_n) {
                auto* r = _ptr;
                _ptr += aligned_n;
                return r;
            }
            return static_cast<std::byte*>(::operator new(n));
        };
        auto deallocate(std::byte* p, size_t n) noexcept -> void {
            if (pointer_in_buffer(p)) {
                n = align_up(n);
                if (p + n == _ptr) _ptr = p;
            }
            else {
                ::operator delete(p);
            }
        };
    };

    // alias for the default
    template <size_t N>
    using SSA_t = simple_static_arena<N, std::max_align_t>;

} // namespace BA_Concurrency

#endif // SIMPLE_STATIC_ARENA_HPP
