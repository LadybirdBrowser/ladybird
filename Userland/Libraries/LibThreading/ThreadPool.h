/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2024, Braydn Moore <braydn.moore@uwaterloo.ca>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Noncopyable.h>
#include <AK/Queue.h>
#include <LibCore/System.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/MutexProtected.h>
#include <LibThreading/Thread.h>

namespace Threading {

template<typename Pool>
struct ThreadPoolLooper {
    IterationDecision next(Pool& pool, bool wait)
    {
        Optional<typename Pool::Work> entry;
        while (true) {
            entry = pool.looper_with_global_queue([&](auto& queue) -> Optional<typename Pool::Work> {
                if (queue.is_empty())
                    return {};
                return queue.dequeue();
            });
            if (entry.has_value())
                break;
            if (pool.looper_should_exit())
                return IterationDecision::Break;

            if (!wait)
                return IterationDecision::Continue;

            pool.looper_wait();
        }

        auto guard = pool.looper_enter_busy_section();
        pool.looper_run_handler([&pool](typename Pool::Work w) { pool.submit(w); }, entry.release_value());
        return IterationDecision::Continue;
    }
};

template<typename TWork, template<typename> class Looper = ThreadPoolLooper>
class ThreadPool {
    AK_MAKE_NONCOPYABLE(ThreadPool);
    AK_MAKE_NONMOVABLE(ThreadPool);

    struct BusyWorkerGuard {
        [[nodiscard]] BusyWorkerGuard(Atomic<size_t>& busy_count, ConditionVariable& work_done)
            : m_busy_count(busy_count)
            , m_work_done(work_done)
        {
            ++m_busy_count;
        }

        ~BusyWorkerGuard()
        {
            --m_busy_count;
            m_work_done.signal();
        }

    private:
        Atomic<size_t>& m_busy_count;
        ConditionVariable& m_work_done;
    };

public:
    using Work = TWork;

    ThreadPool(Optional<size_t> concurrency = {})
    requires(requires(Work w, Function<void(Work)> f) {
        w(f);
    })
        : m_handler([](Function<void(Work)> submit, Work work) { return work(forward(submit)); })
        , m_work_available(m_mutex)
        , m_work_done(m_mutex)
    {
        initialize_workers(concurrency.value_or(Core::System::hardware_concurrency()));
    }

    explicit ThreadPool(Function<void(Function<void(Work)>, Work)> handler, Optional<size_t> concurrency = {})
        : m_handler(move(handler))
        , m_work_available(m_mutex)
        , m_work_done(m_mutex)
    {
        initialize_workers(concurrency.value_or(Core::System::hardware_concurrency()));
    }

    ~ThreadPool()
    {
        m_should_exit.store(true, AK::MemoryOrder::memory_order_release);
        for (auto& worker : m_workers) {
            while (!worker->has_exited()) {
                m_work_available.broadcast();
            }
            (void)worker->join();
        }
    }

    void submit(Work work)
    {
        m_work_queue.with_locked([&](auto& queue) {
            queue.enqueue({ move(work) });
        });
        m_work_available.broadcast();
    }

    void wait_for_all()
    {
        {
            MutexLocker lock(m_mutex);
            m_work_done.wait_while([this]() {
                return m_busy_count.load(AK::MemoryOrder::memory_order_acquire) > 0
                    || m_work_queue.try_with_locked([](auto& queue) { return !queue.is_empty(); }).value_or(true);
            });
        }
    }

    template<typename Func>
    decltype(auto) looper_with_global_queue(Func f)
    {
        return m_work_queue.with_locked(f);
    }

    template<typename Func>
    decltype(auto) looper_try_with_global_queue(Func f)
    {
        return m_work_queue.try_with_locked(f);
    }

    void looper_wait()
    {
        MutexLocker guard(m_mutex);
        m_work_done.signal();
        m_work_available.wait();
    }

    inline bool looper_should_exit() const { return m_should_exit; }
    inline size_t looper_num_workers() const { return m_workers.size(); }
    inline void looper_signal_work_available() { m_work_available.broadcast(); }

    template<typename... Args>
    inline void looper_run_handler(Args&&... args) const { m_handler(forward<Args>(args)...); }

    [[nodiscard]] inline BusyWorkerGuard looper_enter_busy_section() { return BusyWorkerGuard(m_busy_count, m_work_done); }

private:
    void initialize_workers(size_t concurrency)
    {
        for (size_t i = 0; i < concurrency; ++i) {
            m_workers.append(Thread::construct([this]() -> intptr_t {
                Looper<ThreadPool> thread_looper;
                for (; !m_should_exit;) {
                    auto result = thread_looper.next(*this, true);
                    if (result == IterationDecision::Break)
                        break;
                }

                return 0;
            },
                "ThreadPool worker"sv));
        }

        for (auto& worker : m_workers)
            worker->start();
    }

    Vector<NonnullRefPtr<Thread>> m_workers;
    MutexProtected<Queue<Work>> m_work_queue;
    Function<void(Function<void(Work)>, Work)> m_handler;
    Mutex m_mutex;
    ConditionVariable m_work_available;
    ConditionVariable m_work_done;
    Atomic<bool> m_should_exit { false };
    Atomic<size_t> m_busy_count { 0 };
};

}
