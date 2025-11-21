// IThread_Pool.hpp

#ifndef IWORKER_POOL_HPP
#define IWORKER_POOL_HPP

#include <functional>

namespace BA_Concurrency {
    class IThread_Pool {
    public:
        virtual ~IThread_Pool() = default;

        virtual void submit(std::function<void()> job) = 0;
        virtual void shutdown() = 0;
    };
} // namespace BA_Concurrency

#endif // IWORKER_POOL_HPP
