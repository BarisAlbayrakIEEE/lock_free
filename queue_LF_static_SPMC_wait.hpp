#include <cstddef>
#include <array>
#include <atomic>
#include <optional>

#ifndef QUEUE_LF_STATIC_SPMC_WAIT_HPP
#define QUEUE_LF_STATIC_SPMC_WAIT_HPP

template <class T, std::size_t N>
    requires std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_assignable_v<T> &&
             std::is_default_constructible_v<T>
class queue_LF_static_SPMC_wait {
    static_assert(N > 0);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    
    std::array<T, N> _static_buffer{};
    std::atomic<std::size_t> _size{ 0 };
    std::atomic<std::size_t> _index__pop{ 0 };
    std::size_t _index__push{ 0 };

    // strong exception safety
    void push_helper(auto&& t) {
        _size.wait(N, std::memory_order_acquire);

        _static_buffer[_index__push] = std::forward<decltype(t)>(t); // can fail if T's copy/move ctor can throw
        _index__push = (_index__push + 1) % N; // no throw
        _size.fetch_add(1, std::memory_order_release); // no throw
        _size.notify_all(); // no throw
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
        _size.notify_all(); // no throw
    }

    // strong exception safety
    auto pop() -> T {
        while (true) {
            // single producer
            //   -> call wait before CAS for performance
            std::size_t current_size = _size.load(std::memory_order_acquire);
            if (current_size == 0) {
                _size.wait(0, std::memory_order_acquire);
                continue;
            }

            // CAS
            if (
                !_size.compare_exchange_weak(
                    current_size, current_size - 1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                continue; // another consumer took it, retry
            }

            // synchronizes with !_size.compare_exchange_weak
            //   -> synchronizes with the producer's push
            // no popping order in SPMC for the consumer threads
            //   -> memory_order_relaxed
            //
            // CAUTION:
            //   _index__pop is left unbounded
            //   in order to use fetch_add instead of a CAS loop
            //   for performance.
            std::size_t index = _index__pop.fetch_add(1, std::memory_order_relaxed) % N;
            T val = std::move(_static_buffer[index]);
            _size.notify_one();

            // move optimized
            // no throw by std::is_nothrow_move_constructible_v<T>
            return val;
        }
    }

    // not reliable but not needed to be -> memory_order_relaxed
    auto size() const noexcept { return _size.load(std::memory_order_relaxed); }
};

#endif // QUEUE_LF_STATIC_SPMC_WAIT_HPP
