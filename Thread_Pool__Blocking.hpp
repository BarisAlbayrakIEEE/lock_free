// Thread_Pool__Blocking.hpp

#ifndef THREAD_POOL__BLOCKING_HPP
#define THREAD_POOL__BLOCKING_HPP

#include "IThread_Pool.hpp"
#include "Concurrent_Queue__Blocking.hpp"
#include <vector>
#include <thread>
#include <memory>
#include <future>
#include <type_traits>

namespace BA_Concurrency {
    class Thread_Pool__Blocking : public IThread_Pool {
        using job_t = std::function<void()>;
    public:
        explicit Thread_Pool__Blocking(
            size_t thread_count = std::thread::hardware_concurrency())
                : _thread_count(thread_count == 0 ? 1 : thread_count)
        {
            for (size_t i = 0; i < _thread_count; ++i)
                _threads.emplace_back([this] { worker_loop(); });
        }

        ~Thread_Pool__Blocking() {
            if (_running) shutdown();
        }

        void submit(std::function<void()> job) override {
            _jobs.push(std::move(job));
            ++_jobs_in_progress;
        }

        // utility function
        template<typename F, typename... Args>
        auto submit_any(F&& f, Args&&... args) {
            using R = std::invoke_result_t<F, Args...>;

            auto task = std::make_shared<std::packaged_task<R()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));
            auto fut = task->get_future();
            _jobs.push([task]() { (*task)(); });

            ++_jobs_in_progress;
            return fut;
        }

        inline void shutdown() override {
            if (bool expected{true}; !_running.compare_exchange_strong(expected, false))
                return;
            for (auto& t : _threads) t.join();
        }

        inline size_t get_thread_count() const override {
            return _thread_count;
        }

        inline void wait_all_jobs() override {
            std::unique_lock lk(_m);
            _cv.wait(lk, [&]{ return _jobs_in_progress == 0 && _jobs.empty(); });
        }

    private:

        inline void worker_loop() {
            while (true) {
                auto job = _jobs.pop();
                if (!job.has_value()) {
                    if (!_running.load()) break;
                    else continue;
                }
                job.value()();
                --_jobs_in_progress;
                if (_jobs_in_progress == 0)
                    _cv.notify_all();
            }
        }

        Concurrent_Queue__Blocking<job_t> _jobs;
        std::vector<std::thread> _threads;
        size_t _thread_count{};
        std::atomic<size_t> _jobs_in_progress{0};
        std::condition_variable _cv;
        mutable std::mutex _m;
        std::atomic<bool> _running{true};
    };
} // namespace BA_Concurrency

#endif // THREAD_POOL__BLOCKING_HPP
