// IThread_Pool.hpp

#ifndef ITHREAD_POOL_HPP
#define ITHREAD_POOL_HPP

#include <functional>

namespace BA_Concurrency {
    class IThread_Pool {
    public:
        virtual ~IThread_Pool() = default;

        virtual void submit(std::function<void()>) = 0;
        virtual void shutdown() = 0;
        virtual size_t get_thread_count() const = 0;
        virtual void wait_all_jobs() = 0;
    };
} // namespace BA_Concurrency

#endif // ITHREAD_POOL_HPP
