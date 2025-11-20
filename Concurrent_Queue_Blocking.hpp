// Concurrent_Queue_Blocking.hpp

#ifndef CONCURRENT_QUEUE_BLOCKING_HPP
#define CONCURRENT_QUEUE_BLOCKING_HPP

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include "Concurrent_Queue.hpp"
#include "enum_structure_types.hpp"
#include "enum_concurrency_models.hpp"

namespace BA_Concurrency {
    template <typename T>
    class Concurrent_Queue<
        false,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        T> {
    public:
    template <typename U = T>
        void push(U&& data) {
            {
                std::unique_lock lk(_m);
                _queue.push(std::forward<U>(data));
            }
            _cv.notify_one();
        }

        std::optional<T> pop() {
            std::unique_lock lk(_m);
            _cv.wait(lk, [&]{ return !_queue.empty() || _stopped; });
            if (_stopped && _queue.empty())
                return {};

            T data = std::move(_queue.front());
            _queue.pop();
            return data;
        }

        void stop() {
            {
                std::unique_lock lk(_m);
                _stopped = true;
            }
            _cv.notify_all();
        }

    private:
        std::queue<T> _queue;
        std::mutex _m;
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
