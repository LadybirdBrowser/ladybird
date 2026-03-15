/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, kleines Filmröllchen <malu.bertsch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Debug.h>
#include <AK/Noncopyable.h>
#include <AK/Types.h>
#include <pthread.h>

namespace Threading {

class MutexImpl {
    friend class ConditionVariable;

protected:
    void initialize(int type)
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, type);
        auto rc = pthread_mutex_init(&m_mutex, &attr);
        VERIFY(rc == 0);
        pthread_mutexattr_destroy(&attr);
    }

    void destroy()
    {
        auto rc = pthread_mutex_destroy(&m_mutex);
        VERIFY(rc == 0);
    }

    ALWAYS_INLINE void lock()
    {
        auto rc = pthread_mutex_lock(&m_mutex);
        VERIFY(rc == 0);
    }

    ALWAYS_INLINE void unlock()
    {
        auto rc = pthread_mutex_unlock(&m_mutex);
        VERIFY(rc == 0);
    }

private:
    pthread_mutex_t m_mutex;
};

class RecursiveMutex : protected MutexImpl {
    AK_MAKE_NONCOPYABLE(RecursiveMutex);
    AK_MAKE_NONMOVABLE(RecursiveMutex);

public:
    RecursiveMutex() { initialize(PTHREAD_MUTEX_RECURSIVE); }
    ~RecursiveMutex() { destroy(); }

    using MutexImpl::lock;
    using MutexImpl::unlock;
};

class Mutex : protected MutexImpl {
    AK_MAKE_NONCOPYABLE(Mutex);
    AK_MAKE_NONMOVABLE(Mutex);
    friend class ConditionVariable;

public:
    enum Behavior {
        Normal = PTHREAD_MUTEX_NORMAL,
        Checked = PTHREAD_MUTEX_ERRORCHECK
    };

#if MUTEX_DEBUG
    static constexpr Behavior default_behavior = Checked;
#else
    static constexpr Behavior default_behavior = Normal;
#endif

    Mutex(Behavior behavior = default_behavior) { initialize(behavior); }

    ~Mutex() { destroy(); }

    using MutexImpl::lock;
    using MutexImpl::unlock;
};

template<typename T>
concept Lockable = requires(T t) {
    t.lock();
    t.unlock();
};

template<Lockable Mutex>
class [[nodiscard]] MutexLocker {
    AK_MAKE_NONCOPYABLE(MutexLocker);
    AK_MAKE_NONMOVABLE(MutexLocker);

public:
    ALWAYS_INLINE explicit MutexLocker(Mutex& mutex)
        : m_mutex(mutex)
    {
        lock();
    }
    ALWAYS_INLINE ~MutexLocker()
    {
        unlock();
    }
    ALWAYS_INLINE void unlock() { m_mutex.unlock(); }
    ALWAYS_INLINE void lock() { m_mutex.lock(); }

private:
    Mutex& m_mutex;
};

}
