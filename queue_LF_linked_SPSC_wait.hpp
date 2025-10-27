#ifndef QUEUE_LF_LINKED_SPSC_WAIT_HPP
#define QUEUE_LF_LINKED_SPSC_WAIT_HPP

#include <cstddef>
#include <atomic>
#include <optional>

template <class T, std::size_t N>
    requires std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_assignable_v<T> &&
             std::is_default_constructible_v<T>
class queue_LF_linked_SPSC_wait {
    static_assert(N > 0);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    
    std::array<T, N> _static_buffer{};
    std::atomic<std::size_t> _size{ 0 };
    std::size_t _index__pop{ 0 };
    std::size_t _index__push{ 0 };

    // strong exception safety
    void push_helper(auto&& data) {
        _size.wait(N, std::memory_order_acquire);

        _static_buffer[_index__push] = std::forward<decltype(data)>(data); // can fail if T's copy/move ctor can throw
        _index__push = (_index__push + 1) % N; // no throw
        _size.fetch_add(1, std::memory_order_release); // no throw
        _size.notify_one(); // no throw
    }

public:

    // strong exception safety
    void push(T&& data) { push_helper(std::move(data)); }
    // strong exception safety
    void push(const T& data) { push_helper(data); }
    
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

        auto data = std::move(_static_buffer[_index__pop]); // relies on std::is_nothrow_move_assignable_v<T>
        _index__pop = (_index__pop + 1) % N; // no throw
        _size.fetch_sub(1, std::memory_order_release); // no throw
        _size.notify_one(); // no throw

        // NRVO or move optimized
        // no throw by std::is_nothrow_move_constructible_v<T>
        return data;
    }

    // not reliable but not needed to be -> memory_order_relaxed
    auto size() const noexcept { return _size.load(std::memory_order_relaxed); }
};

#endif // QUEUE_LF_LINKED_SPSC_WAIT_HPP
