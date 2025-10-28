#ifndef QUEUE_LF_RING_SPMC_NOWAIT_HPP
#define QUEUE_LF_RING_SPMC_NOWAIT_HPP

#include <cstddef>
#include <array>
#include <atomic>
#include <optional>

template <class T, std::size_t N>
    requires std::is_nothrow_move_constructible_v<T> &&
             std::is_nothrow_move_assignable_v<T> &&
             std::is_default_constructible_v<T>
class queue_LF_ring_SPMC_nowait {
    static_assert(N > 0);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    
    std::array<T, N> _ring_buffer{};
    std::atomic<std::size_t> _size{ 0 };
    std::atomic<std::size_t> _index__pop{ 0 };
    std::size_t _index__push{ 0 };

    // strong exception safety
    bool push_helper(auto&& data) {
        if (_size.load(std::memory_order_acquire) == N) return false;

        _ring_buffer[_index__push] = std::forward<decltype(data)>(data); // can fail if T's copy/move ctor can throw
        _index__push = (_index__push + 1) % N; // no throw
        _size.fetch_add(1, std::memory_order_release); // no throw
        return true;
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
        return true;
    }

    // strong exception safety
    auto pop() -> std::optional<T> {
        std::optional<T> data{};

        // single producer
        //   -> call wait before CAS for performance
        std::size_t current_size = _size.load(std::memory_order_acquire);
        if (current_size > 0) {
            // CAS
            if (
                _size.compare_exchange_weak(
                    current_size, current_size - 1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {

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
                data = std::move(_ring_buffer[index]);
            }
        }

        // NRVO or move optimized
        // no throw by std::is_nothrow_move_constructible_v<T>
        return data;
    }

    // not reliable but not needed to be -> memory_order_relaxed
    auto size() const noexcept { return _size.load(std::memory_order_relaxed); }
};

#endif // QUEUE_LF_RING_SPMC_NOWAIT_HPP
