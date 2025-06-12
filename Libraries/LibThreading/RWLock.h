/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Noncopyable.h>
#include <AK/Types.h>
#include <pthread.h>

namespace Threading {

class RWLock {
    AK_MAKE_NONCOPYABLE(RWLock);
    AK_MAKE_NONMOVABLE(RWLock);

public:
    RWLock()
    {
        pthread_rwlock_init(&m_rwlock, nullptr);
    }

    ~RWLock()
    {
        VERIFY(!m_write_locked);
        pthread_rwlock_destroy(&m_rwlock);
    }

    void lock_read();
    void lock_write();

    void unlock();

private:
    pthread_rwlock_t m_rwlock;
    bool m_write_locked { false };
    bool m_read_locked_with_write_lock { false };
};

enum class LockMode {
    Read,
    Write,
};
template<LockMode mode>
class RWLockLocker {
    AK_MAKE_NONCOPYABLE(RWLockLocker);
    AK_MAKE_NONMOVABLE(RWLockLocker);

public:
    ALWAYS_INLINE explicit RWLockLocker(RWLock& l)
        : m_lock(l)
    {
        lock();
    }

    ALWAYS_INLINE ~RWLockLocker()
    {
        unlock();
    }

    ALWAYS_INLINE void unlock()
    {
        m_lock.unlock();
    }

    ALWAYS_INLINE void lock()
    {
        if constexpr (mode == LockMode::Read)
            m_lock.lock_read();
        else
            m_lock.lock_write();
    }

private:
    RWLock& m_lock;
};

ALWAYS_INLINE void RWLock::lock_read()
{
    auto rc = pthread_rwlock_rdlock(&m_rwlock);
    if (rc == EDEADLK) {
        // We're already holding the write lock, so we can just return.
        m_read_locked_with_write_lock = true;
    } else {
        VERIFY(rc == 0);
    }
}

ALWAYS_INLINE void RWLock::lock_write()
{
    auto rc = pthread_rwlock_wrlock(&m_rwlock);
    VERIFY(rc == 0);
    m_write_locked = true;
}

ALWAYS_INLINE void RWLock::unlock()
{
    m_write_locked = false;
    auto needs_unlock = !m_read_locked_with_write_lock;
    m_read_locked_with_write_lock = false;
    if (needs_unlock)
        pthread_rwlock_unlock(&m_rwlock);
}

}
