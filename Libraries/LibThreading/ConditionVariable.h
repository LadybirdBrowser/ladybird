/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibThreading/Mutex.h>
#include <pthread.h>
#include <sys/types.h>

namespace Threading {

// A signaling condition variable that wraps over the pthread_cond_* APIs.
class ConditionVariable {
    friend class Mutex;

public:
    ConditionVariable(Mutex& to_wait_on)
        : m_to_wait_on(to_wait_on)
    {
        auto result = pthread_cond_init(&m_condition, nullptr);
        VERIFY(result == 0);
    }

    ALWAYS_INLINE ~ConditionVariable()
    {
        auto result = pthread_cond_destroy(&m_condition);
        VERIFY(result == 0);
    }

    // As with pthread APIs, the mutex must be locked or undefined behavior ensues.
    ALWAYS_INLINE void wait()
    {
        auto result = pthread_cond_wait(&m_condition, &m_to_wait_on.m_mutex);
        VERIFY(result == 0);
    }
    ALWAYS_INLINE void wait_while(Function<bool()> condition)
    {
        while (condition())
            wait();
    }
    // Release at least one of the threads waiting on this variable.
    ALWAYS_INLINE void signal()
    {
        auto result = pthread_cond_signal(&m_condition);
        VERIFY(result == 0);
    }
    // Release all of the threads waiting on this variable.
    ALWAYS_INLINE void broadcast()
    {
        auto result = pthread_cond_broadcast(&m_condition);
        VERIFY(result == 0);
    }

private:
    pthread_cond_t m_condition;
    Mutex& m_to_wait_on;
};

}
