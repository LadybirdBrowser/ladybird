/*
 * Copyright (c) 2018-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, kleines Filmröllchen <malu.bertsch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <pthread.h>

namespace Threading {

class Mutex {
    AK_MAKE_NONCOPYABLE(Mutex);
    AK_MAKE_NONMOVABLE(Mutex);
    friend class ConditionVariable;

public:
    Mutex()
        : m_lock_count(0)
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&m_mutex, &attr);
    }
    ~Mutex()
    {
        VERIFY(m_lock_count == 0);
        // FIXME: pthread_mutex_destroy() is not implemented.
    }

    void lock();
    void unlock();
    bool try_lock();

private:
    pthread_mutex_t m_mutex;
    unsigned m_lock_count { 0 };
};

class MutexLocker {
    AK_MAKE_NONCOPYABLE(MutexLocker);
    AK_MAKE_NONMOVABLE(MutexLocker);

public:
    ALWAYS_INLINE explicit MutexLocker(Mutex& mutex, bool already_locked = false)
        : m_mutex(mutex)
    {
        if (!already_locked) {
            lock();
        }
    }
    ALWAYS_INLINE ~MutexLocker()
    {
        unlock();
    }
    ALWAYS_INLINE void unlock() { m_mutex.unlock(); }
    ALWAYS_INLINE void lock() { m_mutex.lock(); }

    [[nodiscard]] static AK::Optional<MutexLocker> try_lock(Mutex& m)
    {
        if (m.try_lock()) {
            return AK::Optional<MutexLocker>(AK::Detail::InPlace, m, true);
        }

        return {};
    }

private:
    Mutex& m_mutex;
};

ALWAYS_INLINE void Mutex::lock()
{
    pthread_mutex_lock(&m_mutex);
    m_lock_count++;
}

ALWAYS_INLINE void Mutex::unlock()
{
    VERIFY(m_lock_count > 0);
    // FIXME: We need to protect the lock count with the mutex itself.
    // This may be bad because we're not *technically* unlocked yet,
    // but we're not handling any errors from pthread_mutex_unlock anyways.
    m_lock_count--;
    pthread_mutex_unlock(&m_mutex);
}

ALWAYS_INLINE bool Mutex::try_lock()
{
    bool success = pthread_mutex_trylock(&m_mutex) == 0;
    if (success) {
        ++m_lock_count;
    }

    return success;
}

}
