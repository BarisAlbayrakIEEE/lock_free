// Thread_Pool__LF.hpp

#ifndef THREAD_POOL__LF_HPP
#define THREAD_POOL__LF_HPP

#include "IThread_Pool.hpp"
#include "Concurrent_Queue__LF_Ring_MPMC.hpp"
#include "tp_util.hpp"
#include <vector>
#include <thread>
#include <atomic>

namespace BA_Concurrency {
    class Thread_Pool__LF : public IThread_Pool {
        using job_t = std::function<void()>;
        using jobs_t = queue_LF_ring_MPMC<job_t, Capacity_As_Pow2>;
    public:
        explicit Thread_Pool__LF(
            size_t thread_count = std::thread::hardware_concurrency())
                : _thread_count(thread_count == 0 ? 1 : thread_count)
        {
            for (size_t i = 0; i < thread_count; ++i) {
                _threads.emplace_back([this] {
                    while (_running.load(std::memory_order_relaxed)) {
                        auto job = _jobs.try_pop();
                        if (job.has_value()) {
                            job.value()();
                        } else {
                            std::this_thread::yield();
                        }
                    }
                });
            }
        }

        ~Thread_Pool__LF() {
            if (_running) shutdown();
        }

        inline void submit(job_t job) override {
            _jobs.push(std::move(job));
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
            /*
            TODO: wait_all_jobs
            */
            ;
        }

    private:
        jobs_t _jobs;
        std::vector<std::thread> _threads;
        size_t _thread_count{};
        std::atomic<bool> _running{true};
        std::atomic<size_t> _jobs_in_progress{0};
    };
} // namespace BA_Concurrency

#endif // THREAD_POOL__LF_HPP
