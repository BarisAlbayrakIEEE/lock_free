// Concurrent_Queue_Blocking.hpp

#ifndef CONCURRENT_QUEUE_BLOCKING_HPP
#define CONCURRENT_QUEUE_BLOCKING_HPP

#include <queue>
#include <mutex>
#include <condition_variable>
#include "IConcurrent_Queue.hpp"
#include "Concurrent_Queue.hpp"
#include "enum_structure_types.hpp"
#include "enum_concurrency_models.hpp"

namespace BA_Concurrency {
    template <typename T>
    class Concurrent_Queue<
        false,
        Enum_Structure_Types::Linked,
        Enum_Concurrency_Models::MPMC,
        T> : public IConcurrent_Queue<T> {
    public:
        inline void push(const T& data) override {
            push_helper(data);
        }
        inline void push(T&& data) override {
            push_helper(std::move(data));
        }

        std::optional<T> pop() override {
            std::unique_lock lk(_m);
            _cv.wait(lk, [&]{ return !_queue.empty() || _stopped; });
            if (_queue.empty())
                return {};

            T data = std::move(_queue.front());
            _queue.pop();
            return data;
        }

        std::optional<T> try_pop() override {
            std::unique_lock lk(_m);
            if (_queue.empty())
                return {};

            T data = std::move(_queue.front());
            _queue.pop();
            return data;
        }

        inline void stop() {
            {
                std::unique_lock lk(_m);
                _stopped = true;
            }
            _cv.notify_all();
        }

    private:

        template <typename U = T>
        inline void push_helper(U&& data) {
            {
                std::unique_lock lk(_m);
                _queue.push(std::forward<U>(data));
            }
            _cv.notify_one();
        }

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
