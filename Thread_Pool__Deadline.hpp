// Thread_Pool__Deadline.hpp

#ifndef THREAD_POOL__DEADLINE_HPP
#define THREAD_POOL__DEADLINE_HPP

#include "IThread_Pool.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>

namespace BA_Concurrency {
    class Thread_Pool__Deadline : public IThread_Pool {
        using job_t = std::function<void()>;

        struct Deadline_Job {
            std::chrono::steady_clock::time_point _deadline;
            job_t _job;

            inline bool operator>(const Deadline_Job& rhs) const {
                return _deadline > rhs._deadline;
            }
        };

    public:

        explicit Thread_Pool__Deadline(
            size_t thread_count = std::thread::hardware_concurrency())
                : _thread_count(thread_count == 0 ? 1 : thread_count)
        {
            for (size_t i = 0; i < thread_count; ++i)
                _threads.emplace_back([this] { worker_loop(); });
        }

        ~Thread_Pool__Deadline() {
            if (_running) shutdown();
        }

        inline void submit(job_t job) override {
            Deadline_Job dj{ std::chrono::steady_clock::now(), std::move(job) };
            {
                std::scoped_lock lk(_m);
                _djs.push(std::move(dj));
            }
            _cv.notify_one();
        }

        inline void shutdown() override {
            if (bool expected{true}; !_running.compare_exchange_strong(expected, false))
                return;
            _cv.notify_all();
            for (auto& t : _threads) t.join();
        }

        inline size_t get_thread_count() const override {
            return _thread_count;
        }

        inline void wait_all_jobs() override {
            /*
            TODO: wait_all_jobs
            */
            ;
        }

    private:

        void worker_loop() {
            while (_running) {
                Deadline_Job dj;
                {
                    std::unique_lock lk(_m);
                    _cv.wait(lk, [&]{ return !_djs.empty() || !_running; });
                    if (!_running) break;

                    dj = std::move(_djs.top());
                    _djs.pop();
                }
                dj._job();
            }
        }

        std::priority_queue<Deadline_Job, std::vector<Deadline_Job>, std::greater<>> _djs;
        std::vector<std::thread> _threads;
        size_t _thread_count{};
        std::condition_variable _cv;
        std::mutex _m;
        std::atomic<bool> _running{true};
    };
} // namespace BA_Concurrency

#endif // THREAD_POOL__DEADLINE_HPP
