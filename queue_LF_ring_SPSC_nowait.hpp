#ifndef QUEUE_LF_RING_SPSC_NOWAIT_HPP
#define QUEUE_LF_RING_SPSC_NOWAIT_HPP

#include <cstddef>
#include <array>
#include <atomic>
#include <optional>

template <class T, std::size_t N>
    requires std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_assignable_v<T> &&
             std::is_default_constructible_v<T>
class queue_LF_ring_SPSC_nowait {
    static_assert(N > 0);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    
    std::array<T, N> _ring_buffer{};
    std::atomic<std::size_t> _size{ 0 };
    std::size_t _index__pop{ 0 };
    std::size_t _index__push{ 0 };

    // strong exception safety
    bool push_helper(auto&& data) {
        if (_size.load(std::memory_order_acquire) == N) return false;

        _ring_buffer[_index__push] = std::forward<decltype(data)>(data); // can fail if T's copy/move ctor can throw
        _index__push = (_index__push + 1) % N; // no throw
        _size.fetch_add(1, std::memory_order_release); // no throw
        return true; // no throw
    }

public:

    // strong exception safety
    bool push(T&& data) { return push_helper(std::move(data)); }
    // strong exception safety
    bool push(const T& data) { return push_helper(data); }
    
    // strong exception safety
    template<typename... Ts>
    bool emplace(Ts&&... args) {
        if (_size.load(std::memory_order_acquire) == N) return false;

        // can fail if T's ctor can throw
        // would be optimized by the compiler
        _ring_buffer[_index__push] = T(std::forward<Ts>(args)...);
        _index__push = (_index__push + 1) % N; // no throw
        _size.fetch_add(1, std::memory_order_release); // no throw
        return true; // no throw
    }

    // strong exception safety
    auto pop() -> std::optional<T> {
        std::optional<T> data{};
        if (_size.load(std::memory_order_acquire) > 0) {
            data = std::move(_ring_buffer[_index__pop]); // relies on std::is_nothrow_move_assignable_v<T>
            _index__pop = (_index__pop + 1) % N; // no throw
            _size.fetch_sub(1, std::memory_order_release); // no throw
        }
        return data; // no throw by std::is_nothrow_move_constructible_v<T>
    }

    // not reliable but not needed to be -> memory_order_relaxed
    auto size() const noexcept { return _size.load(std::memory_order_relaxed); }
};

#endif // QUEUE_LF_RING_SPSC_NOWAIT_HPP
