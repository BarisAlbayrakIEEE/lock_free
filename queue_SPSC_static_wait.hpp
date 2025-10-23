#include <cstddef>
#include <array>
#include <atomic>
#include <optional>

#ifndef QUEUE_SPSC_STATIC_WAIT_HPP
#define QUEUE_SPSC_STATIC_WAIT_HPP

template <class T, std::size_t N>
    requires std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_assignable_v<T> &&
             std::is_default_constructible_v<T>
class queue_SPSC_static_wait {
    static_assert(N > 0);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    
    std::array<T, N> _static_buffer{};
    std::atomic<std::size_t> _size{ 0 };
    std::size_t _index__pop{ 0 };
    std::size_t _index__push{ 0 };

    // strong exception safety
    void push_helper(auto&& t) {
        _size.wait(N, std::memory_order_acquire);

        _static_buffer[_index__push] = std::forward<decltype(t)>(t); // can fail if T's copy/move ctor can throw
        _index__push = (_index__push + 1) % N; // no throw
        _size.fetch_add(1, std::memory_order_release); // no throw
        _size.notify_one(); // no throw
    }

public:

    // strong exception safety
    void push(T&& t) { push_helper(std::move(t)); }
    // strong exception safety
    void push(const T& t) { push_helper(t); }
    
    // strong exception safety
    template<typename... Ts>
    void emplace(Ts&&... args) {
        _size.wait(N, std::memory_order_acquire);

        // can fail if T's ctor can throw
        // would be optimized by the compiler
        _static_buffer[_index__push] = T(std::forward<Ts>(args)...);
        _index__push = (_index__push + 1) % N; // no throw
        _size.fetch_add(1, std::memory_order_release); // no throw
        _size.notify_one(); // no throw
    }

    // strong exception safety
    auto pop() -> T {
        _size.wait(0, std::memory_order_acquire);

        auto val = std::move(_static_buffer[_index__pop]); // relies on std::is_nothrow_move_assignable_v<T>
        _index__pop = (_index__pop + 1) % N; // no throw
        _size.fetch_sub(1, std::memory_order_release); // no throw
        _size.notify_one(); // no throw

        // NRVO or move optimized
        // no throw by std::is_nothrow_move_constructible_v<T>
        return val;
    }

    // not reliable but not needed to be -> memory_order_relaxed
    auto size() const noexcept { return _size.load(std::memory_order_relaxed); }
};

#endif // QUEUE_SPSC_STATIC_WAIT_HPP
