// Thread_Pool__Actor.hpp

#ifndef THREAD_POOL__ACTOR_HPP
#define THREAD_POOL__ACTOR_HPP

#include "IThread_Pool.hpp"
#include "Concurrent_Queue__LF_Ring_MPSC.hpp"
#include "tp_util.hpp"
#include <vector>
#include <thread>
#include <atomic>

namespace BA_Concurrency {
    class Actor_Ref;
    class Thread_Pool__Actor;
    using msg_t = std::function<void(Actor_Ref)>;
    using mailbox_t = queue_LF_ring_MPSC<msg_t, Capacity_As_Pow2>;

    class Actor_Ref {
        friend class Thread_Pool__Actor;
    public:
        Actor_Ref() : _id(SIZE_MAX), _mailboxs(nullptr) {}
        Actor_Ref(size_t id, std::vector<mailbox_t>* messages) : _id(id), _mailboxs(messages) {}

        bool valid() const {
            return _mailboxs && _id != SIZE_MAX;
        }
        size_t id() const { return _id; }

        template <typename F>
        inline void send_self(F&& f) const {
            if (!valid()) return;
            (*_mailboxs)[_id].push(msg_t([job = std::forward<F>(f)](Actor_Ref self) { job(self); }));
        }
        template <typename F>
        void send_to(size_t id, F&& f) const {
            if (!valid())
                return;
            (*_mailboxs)[id].push(msg_t([job = std::forward<F>(f)](Actor_Ref self) { job(self); }));
        }

    private:
        size_t _id;
        std::vector<mailbox_t>* _mailboxs;
    };

    class Thread_Pool__Actor {
    public:
        explicit Thread_Pool__Actor(size_t thread_count = std::thread::hardware_concurrency())
            : _thread_count(thread_count == 0 ? 1 : thread_count)
        {
            _mailboxs.resize(thread_count);
            _actor_refs.reserve(thread_count);
            for (size_t i = 0; i < thread_count; ++i)
                _actor_refs.emplace_back(i, &_mailboxs);
            for (size_t i = 0; i < thread_count; ++i)
                _workers.emplace_back([this, i] { worker_loop(i); });
        }

        ~Thread_Pool__Actor() {
            shutdown();
        }

        inline Actor_Ref get_actor_ref(size_t i) const {
            return _actor_refs[i];
        }

        void submit(std::function<void()> job) override {
            size_t id = _next.fetch_add(1, std::memory_order_relaxed) % _mailboxs.size();
            _mailboxs[id].push(msg_t( { job(); }));
        }

        template <typename F>
            requires std::invocable<F, Actor_Ref>
        inline void submit(F&& f) {
            size_t id = _next.fetch_add(1, std::memory_order_relaxed) % _mailboxs.size();
            _mailboxs[id].push(msg_t([job = std::forward<F>(f)](Actor_Ref self) { job(self); }));
        }

        inline void shutdown() {
            bool expected = true;
            if (!_running.compare_exchange_strong(expected, false))
                return;
            for (auto& t : _workers) if (t.joinable()) t.join();
        }

        inline size_t get_thread_count() const override {
            return _thread_count;
        }

    private:

        inline void worker_loop(size_t id_self) {
            Actor_Ref self = _actor_refs[id_self];
            while (_running.load(std::memory_order_relaxed)) {
                auto msg = _mailboxs[id_self].try_pop();
                if (msg) msg.value()(self);
                else std::this_thread::yield();
            }
        }

        std::vector<mailbox_t> _mailboxs;
        std::vector<Actor_Ref> _actor_refs;
        std::vector<std::thread> _workers;
        size_t _thread_count{};
        std::atomic<size_t> _next{0};
        std::atomic<bool> _running{true};
    };
}

#endif // THREAD_POOL__ACTOR_HPP
