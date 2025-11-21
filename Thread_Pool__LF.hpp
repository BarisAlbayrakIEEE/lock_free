// Thread_Pool__LFh

#ifndef WORKER_POOL__LF_HPP
#define WORKER_POOL__LF_HPP

#include "IThread_Pool.hpp"
#include "Concurrent_Queue_LF_Ring_MPMC.hpp"
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

    private:
        jobs_t _jobs;
        std::vector<std::thread> _threads;
        std::atomic<bool> _running{true};
    };
} // namespace BA_Concurrency

#endif // WORKER_POOL__LF_HPP
