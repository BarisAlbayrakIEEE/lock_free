#ifndef SIMPLE_STATIC_ARENA_HPP
#define SIMPLE_STATIC_ARENA_HPP

#include <cstddef>
#include <cstdint>

namespace BA_Concurrency {
    template <size_t N, size_t Alignment = alignof(std::max_align_t)>
    class Simple_Static_Arena {
        static constexpr size_t alignment = Alignment;
        static auto align_up(size_t n) noexcept -> size_t {
            return (n + (alignment - 1)) & ~(alignment - 1);
        }
        auto pointer_in_buffer(const std::byte* p) const noexcept -> bool {
            return
                uintptr_t(p) >= uintptr_t(_buffer) &&
                uintptr_t(p) < uintptr_t(_buffer) + N;
        }
        alignas(alignment) std::byte _buffer[N];
        std::byte* _ptr{};

    public:

        Simple_Static_Arena() noexcept : _ptr(_buffer) {}
        Simple_Static_Arena(const Simple_Static_Arena&) = delete;
        Simple_Static_Arena& operator=(const Simple_Static_Arena&) = delete;
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
    using SSA_t = Simple_Static_Arena<N>;
} // namespace BA_Concurrency

#endif // SIMPLE_STATIC_ARENA_HPP
