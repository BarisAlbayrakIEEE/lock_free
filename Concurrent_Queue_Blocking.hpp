// Concurrent_Queue_Blocking.hpp

#ifndef CONCURRENT_QUEUE_BLOCKING_HPP
#define CONCURRENT_QUEUE_BLOCKING_HPP

#include <queue>
#include <mutex>
#include <condition_variable>
#include "Concurrent_Queue.hpp"
#include "enum_structure_types.hpp"
#include "enum_concurrency_models.hpp"

namespace BA_Concurrency {
    template <
        typename T,
        unsigned char Capacity_As_Pow2>
    requires (
            std::is_nothrow_constructible_v<T> &&
            std::is_nothrow_move_constructible_v<T>)
    class Concurrent_Queue<
        false,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        T> {
    public:
        void push(T value) {
            {
                std::unique_lock lock(_mtx);
                _queue.push(std::move(value));
            }
            _cv.notify_one();
        }

        T pop() {
            std::unique_lock lock(_mtx);
            _cv.wait(lock, [&]{ return !_queue.empty() || _stopped; });

            if (_stopped && _queue.empty())
                return {};

            T v = std::move(_queue.front());
            _queue.pop();
            return v;
        }

        void stop() {
            {
                std::unique_lock lock(_mtx);
                _stopped = true;
            }
            _cv.notify_all();
        }

    private:
        std::queue<T> _queue;
        std::mutex _mtx;
        std::condition_variable _cv;
        bool _stopped = false;
    };

    template <typename T>
    using Concurrent_Queue_Blocking = Concurrent_Queue<
        false,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        T>;
} // namespace BA_Concurrency

#endif // CONCURRENT_QUEUE_BLOCKING_HPP
