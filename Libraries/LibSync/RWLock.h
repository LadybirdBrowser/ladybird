/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <LibSync/Export.h>

#if !defined(AK_OS_WINDOWS)
#    include <pthread.h>
#endif
namespace Sync {

// TODO: Implement interprocess RWLocks. This needs a hand-rolled implementation for win32.
class SYNC_API RWLock {
    AK_MAKE_NONCOPYABLE(RWLock);
    AK_MAKE_NONMOVABLE(RWLock);

public:
    RWLock();
    ~RWLock();

    bool try_lock_read();
    bool try_lock_write();

    // Recursively acquiring a RWLock is not supported
    void lock_read();
    void lock_write();

    // NOTE: While the pthread api has one unlock method, the Win32 api has separate ones per lock mode

    void unlock_read();
    void unlock_write();

private:
#ifdef AK_OS_WINDOWS
    using StorageType = void*;
#else
    using StorageType = pthread_rwlock_t;
#endif

    alignas(StorageType) unsigned char m_storage[sizeof(StorageType)];
};

enum class LockMode : u8 {
    Read,
    Write,
};

template<LockMode mode>
class SYNC_API RWLockLocker {
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
        if constexpr (mode == LockMode::Read)
            m_lock.unlock_read();
        else
            m_lock.unlock_write();
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

}
