// Thread_Pool__Work_Stealing.hpp

#ifndef WORKER_POOL__WORK_STEALING_HPP
#define WORKER_POOL__WORK_STEALING_HPP

#include "IThread_Pool.hpp"
#include <deque>
#include <mutex>
#include <thread>
#include <future>
#include <vector>
#include <atomic>
#include <optional>

namespace BA_Concurrency {
    class Thread_Pool__Work_Stealing : public IThread_Pool {
        using job_t = std::function<void()>;

        struct Job_Deque {
            std::deque<job_t> _jd;
            std::mutex _m;
        };

    public:

        Thread_Pool__Work_Stealing(
            size_t thread_count = std::thread::hardware_concurrency())
            : _jds(thread_count)
        {
            for (size_t i = 0; i < thread_count; ++i)
                _threads.emplace_back([this, i] { worker_loop(i); });
        }

        ~Thread_Pool__Work_Stealing() {
            if (_running) shutdown();
        }

        template <typename F, typename... Args>
        auto submit(F&& f, Args&&... args)
            -> std::future<std::invoke_result_t<F, Args...>>
        {
            using R = std::invoke_result_t<F, Args...>;

            auto task = std::make_shared<std::packaged_task<R()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));
            auto fut = task->get_future();
            size_t id = _next.fetch_add(1, std::memory_order_relaxed) % _jds.size();
            auto& jd = _jds[id];
            {
                std::scoped_lock lk(jd._m);
                jd._jd.push_back(std::move([task]() { (*task)(); }));
            }

            return fut;
        }

        inline void shutdown() override {
            if (bool expected{true}; !_running.compare_exchange_strong(expected, false))
                return;
            for (auto& jd : _jds) std::scoped_lock lk(jd._m);
            for (auto& t : _threads) if (t.joinable()) t.join();
        }

    private:
    
        std::optional<job_t> steal(size_t thief) {
            size_t n = _jds.size();
            for (size_t i = 0; i < n; ++i) {
                size_t victim = (thief + i) % n;
                if (victim == thief) continue;

                std::scoped_lock lk(_jds[victim]._m);
                if (!_jds[victim]._jd.empty()) {
                    auto job = std::move(_jds[victim]._jd.front());
                    _jds[victim]._jd.pop_front();
                    return job;
                }
            }
            return std::nullopt;
        }

        void worker_loop(size_t id) {
            auto& jd = _jds[id];
            while (_running) {
                job_t job;
                bool has_job = false;
                {
                    std::scoped_lock lk(jd._m);
                    if (!jd._jd.empty()) {
                        job = std::move(jd._jd.back());
                        jd._jd.pop_back();
                        has_job = true;
                    }
                }
                if (!has_job) {
                    auto stolen = steal(id);
                    if (stolen) {
                        job = *std::move(stolen);
                        has_job = true;
                    }
                }
                if (has_job)
                    job();
                else
                    std::this_thread::yield();
            }
        }

        std::vector<Job_Deque> _jds;
        std::vector<std::thread> _threads;
        std::atomic<size_t> _next{0};
        std::atomic<bool> _running{true};
    };
} // namespace BA_Concurrency

#endif // WORKER_POOL__WORK_STEALING_HPP
