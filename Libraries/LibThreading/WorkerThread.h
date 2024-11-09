/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Debug.h>
#include <AK/Variant.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/Thread.h>

namespace Threading {

// Macro to allow single-line logging prints with fields that only exist in debug mode.
#if WORKER_THREAD_DEBUG
#    define WORKER_LOG(args...) ({ dbgln(args); })
#else
#    define WORKER_LOG(args...)
#endif

template<typename ErrorType>
class WorkerThread {
    enum class State {
        Idle,
        Working,
        Stopped,
    };
    using WorkerTask = Function<ErrorOr<void, ErrorType>()>;
    using WorkerState = Variant<State, WorkerTask, ErrorType>;

public:
    static ErrorOr<NonnullOwnPtr<WorkerThread>> create(StringView name)
    {
        auto worker_thread = TRY(adopt_nonnull_own_or_enomem(new (nothrow) WorkerThread()));
        worker_thread->m_thread = TRY(Threading::Thread::try_create([&self = *worker_thread]() {
            WORKER_LOG("Starting worker loop {}", self.m_id);

            while (true) {
                self.m_mutex.lock();
                if (self.m_stop) {
                    WORKER_LOG("Exiting {}", self.m_id);
                    self.m_state = State::Stopped;
                    self.m_condition.broadcast();
                    self.m_mutex.unlock();
                    return 0;
                }
                if (self.m_state.template has<WorkerTask>()) {
                    auto task = move(self.m_state.template get<WorkerTask>());
                    self.m_state = State::Working;
                    self.m_mutex.unlock();

                    WORKER_LOG("Starting task on {}", self.m_id);
                    auto result = task();
                    if (result.is_error()) {
                        WORKER_LOG("Task finished on {} with error", self.m_id);
                        self.m_mutex.lock();
                        self.m_state = result.release_error();
                        self.m_condition.broadcast();
                    } else {
                        WORKER_LOG("Task finished successfully on {}", self.m_id);
                        self.m_mutex.lock();
                        self.m_state = State::Idle;
                        self.m_condition.broadcast();
                    }
                }
                WORKER_LOG("Awaiting new task in {}...", self.m_id);
                self.m_condition.wait();
                WORKER_LOG("Worker thread awoken in {}", self.m_id);
                self.m_mutex.unlock();
            }

            return 0;
        },
            name));
        worker_thread->m_thread->start();
        return worker_thread;
    }

    ~WorkerThread()
    {
        m_mutex.lock();
        m_stop = true;
        m_condition.broadcast();
        while (!is_in_state(State::Stopped))
            m_condition.wait();
        m_mutex.unlock();
        (void)m_thread->join();
        WORKER_LOG("Worker thread {} joined successfully", m_id);
    }

    // Returns whether the task is starting.
    bool start_task(WorkerTask&& task)
    {
        m_mutex.lock();
        VERIFY(!is_in_state(State::Stopped));

        bool start_work = false;
        if (is_in_state(State::Idle)) {
            start_work = true;
        } else if (m_state.template has<ErrorType>()) {
            WORKER_LOG("Starting task and ignoring previous error: {}", m_state.template get<ErrorType>().string_literal());
            start_work = true;
        }
        if (start_work) {
            WORKER_LOG("Queuing task on {}", m_id);
            m_state = move(task);
            m_condition.broadcast();
        }

        m_mutex.unlock();
        return start_work;
    }

    ErrorOr<void, ErrorType> wait_until_task_is_finished()
    {
        WORKER_LOG("Waiting for task to finish on {}...", m_id);
        m_mutex.lock();
        while (true) {
            if (m_state.template has<WorkerTask>() || is_in_state(State::Working)) {
                m_condition.wait();
            } else if (m_state.template has<ErrorType>()) {
                auto error = move(m_state.template get<ErrorType>());
                m_state = State::Idle;
                m_mutex.unlock();
                WORKER_LOG("Finished waiting with error on {}: {}", m_id, error.string_literal());
                return error;
            } else {
                m_mutex.unlock();
                WORKER_LOG("Finished waiting on {}", m_id);
                return {};
            }
        }
        m_mutex.unlock();
    }

private:
#if WORKER_THREAD_DEBUG
    static inline size_t current_id = 0;
#endif

    WorkerThread()
        : m_condition(m_mutex)
#if WORKER_THREAD_DEBUG
        , m_id(current_id++)
#endif
    {
    }
    WorkerThread(WorkerThread const&) = delete;
    WorkerThread(WorkerThread&&) = delete;

    // Must be called with the mutex locked.
    bool is_in_state(State state)
    {
        return m_state.template has<State>() && m_state.template get<State>() == state;
    }

    RefPtr<Threading::Thread> m_thread;
    Threading::Mutex m_mutex;
    Threading::ConditionVariable m_condition;
    WorkerState m_state { State::Idle };
    bool m_stop { false };
#if WORKER_THREAD_DEBUG
    size_t m_id;
#endif
};

#undef WORKER_LOG

}
