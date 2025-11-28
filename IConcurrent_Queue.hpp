// IConcurrent_Queue.hpp

#ifndef ICONCURRENT_QUEUE_HPP
#define ICONCURRENT_QUEUE_HPP

#include <optional>

namespace BA_Concurrency {
    template <typename T>
    class IConcurrent_Queue {
    public:
        virtual ~IConcurrent_Queue() = default;

        virtual void push(T data) = 0;
        virtual std::optional<T> pop() = 0;
        virtual std::optional<T> try_pop() = 0;
        virtual size_t size() const = 0;
        virtual bool empty() const = 0;
    };
} // namespace BA_Concurrency

#endif // ICONCURRENT_QUEUE_HPP
