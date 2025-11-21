// Thread_Pool__Concurrent_Queue_Blocking.hpp

#ifndef WORKER_POOL__BLOCKING_QUEUE_HPP
#define WORKER_POOL__BLOCKING_QUEUE_HPP

#include "IThread_Pool.hpp"
#include "Concurrent_Queue_Blocking.hpp"
#include <vector>
#include <thread>
#include <memory>
#include <future>
#include <type_traits>

namespace BA_Concurrency {
    class Thread_Pool__Concurrent_Queue_Blocking : public IThread_Pool {
        using func_t = std::function<void()>;
    public:
        explicit Thread_Pool__Concurrent_Queue_Blocking(
            size_t thread_count = std::thread::hardware_concurrency())
        {
            for (size_t i = 0; i < thread_count; ++i)
                _threads.emplace_back([this] { worker_loop(); });
        }

        ~Thread_Pool__Concurrent_Queue_Blocking() {
            if (_running) shutdown();
        }

        template<typename F, typename... Args>
        auto submit(F&& f, Args&&... args) {
            using R = std::invoke_result_t<F, Args...>;

            auto task = std::make_shared<std::packaged_task<R()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));
            auto fut = task.get_future();
            _jobs.push([task]() { (*task)(); });

            return fut;
        }

        inline void shutdown() override {
            if (bool expected{true}; !_running.compare_exchange_strong(expected, false))
                return;
            _jobs.stop();
            for (auto& t : _threads) t.join();
        }

    private:

        inline void worker_loop() {
            auto job = _jobs.pop();
            while (_running.load() || job.has_value()) {
                job.value()();
            }
        }

        Concurrent_Queue_Blocking<func_t> _jobs;
        std::vector<std::thread> _threads;
        std::atomic<bool> _running{true};
    };
} // namespace BA_Concurrency

#endif // WORKER_POOL__BLOCKING_QUEUE_HPP
