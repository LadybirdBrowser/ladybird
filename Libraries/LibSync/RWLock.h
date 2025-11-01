/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Noncopyable.h>
#include <AK/Types.h>
#include <LibSync/Export.h>
#include <LibSync/Policy.h>

namespace Sync {

namespace Detail {

struct RWLockImpl;

}

class SYNC_API PlatformRWLock {
    AK_MAKE_NONCOPYABLE(PlatformRWLock);
    AK_MAKE_NONMOVABLE(PlatformRWLock);

public:
    bool try_lock_read();
    bool try_lock_write();

    // Recursively acquiring a RWLock is not supported
    void lock_read();
    void lock_write();

    void unlock();

protected:
    PlatformRWLock();
    virtual ~PlatformRWLock();

    Detail::RWLockImpl& impl() { return *reinterpret_cast<Detail::RWLockImpl*>(m_storage); }
    Detail::RWLockImpl const& impl() const { return *reinterpret_cast<Detail::RWLockImpl const*>(m_storage); }

private:
#ifdef AK_OS_WINDOWS
    static constexpr u64 PTHREAD_RWLOCK_SIZE = 8;
#elifdef AK_OS_MACOS
    static constexpr u64 PTHREAD_RWLOCK_SIZE = 200;
#else
    static constexpr u64 PTHREAD_RWLOCK_SIZE = 56;
#endif
    alignas(void*) char m_storage[PTHREAD_RWLOCK_SIZE];
};

template<typename InterprocessPolicy>
class SYNC_API RWLockBase : public PlatformRWLock {
public:
    RWLockBase();

    virtual ~RWLockBase() = default;
};

using RWLock = RWLockBase<PolicyIntraprocess>;
using InterprocessRWLock = RWLockBase<PolicyInterprocess>;

enum class LockMode : u8 {
    Read,
    Write,
};

template<LockMode mode>
class SYNC_API RWLockLocker {
    AK_MAKE_NONCOPYABLE(RWLockLocker);
    AK_MAKE_NONMOVABLE(RWLockLocker);

public:
    ALWAYS_INLINE explicit RWLockLocker(PlatformRWLock& l)
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
    PlatformRWLock& m_lock;
};

}
