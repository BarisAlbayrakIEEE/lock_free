// Thread_Pool__NUMA_Aware.hpp

#ifndef THREAD_POOL__NUMA_AWARE_HPP
#define THREAD_POOL__NUMA_AWARE_HPP

#include "IThread_Pool.hpp"
#include "Concurrent_Queue__Blocking.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <sched.h>
#include <unistd.h>

namespace BA_Concurrency {
    class Thread_Pool__NUMA_Aware : public IThread_Pool {
        using job_t = std::function<void()>;
    public:
        explicit Thread_Pool__NUMA_Aware(size_t thread_count_per_node = 2) {
            size_t thread_count_per_node_ = thread_count_per_node == 0 ? 1 : thread_count_per_node;
            size_t node_count = std::thread::hardware_concurrency() / thread_count_per_node_;
            if (node_count == 0) node_count = 1;
            _thread_count = node_count * thread_count_per_node_;

            _jobs.resize(node_count);
            for (size_t n = 0; n < node_count; ++n) {
                for (size_t t = 0; t < thread_count_per_node_; ++t) {
                    _threads.emplace_back([this, n] {
                        pin_to_numa_node(n);
                        auto& q = _jobs[n];
                        while (_running) {
                            auto job = q.pop();
                            if (job) job.value()();
                        }
                    });
                }
            }
        }

        ~Thread_Pool__NUMA_Aware() {
            if (_running) shutdown();
        }

        inline void submit(job_t job) override {
            size_t idx = _next++ % _jobs.size();
            _jobs[idx].push(std::move(job));
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

        static void pin_to_numa_node(size_t node) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(node, &set);
            sched_setaffinity(0, sizeof(set), &set);
        }

        std::vector<Concurrent_Queue__Blocking<job_t>> _jobs;
        std::vector<std::thread> _threads;
        size_t _thread_count{};
        std::atomic<size_t> _next{0};
        std::atomic<bool> _running{true};
    };
} // namespace BA_Concurrency

#endif // THREAD_POOL__NUMA_AWARE_HPP
