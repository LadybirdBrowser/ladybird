/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, kleines Filmröllchen <malu.bertsch@gmail.com>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <LibSync/Export.h>
#include <LibSync/Policy.h>

namespace Sync {

namespace Detail {

struct MutexImpl;

}

class SYNC_API PlatformMutex {
    AK_MAKE_NONCOPYABLE(PlatformMutex);
    AK_MAKE_NONMOVABLE(PlatformMutex);
    friend class ConditionVariable;

public:
    void try_lock();
    void lock();
    void unlock();

protected:
    PlatformMutex();
    virtual ~PlatformMutex();

    Detail::MutexImpl& impl() { return *reinterpret_cast<Detail::MutexImpl*>(m_storage); }
    Detail::MutexImpl const& impl() const { return *reinterpret_cast<Detail::MutexImpl const*>(m_storage); }

private:
#ifdef AK_OS_WINDOWS
    static constexpr u64 PTHREAD_MUTEX_SIZE = 8;
#elifdef AK_OS_MACOS
    static constexpr u64 PTHREAD_MUTEX_SIZE = 64;
#else
    static constexpr u64 PTHREAD_MUTEX_SIZE = 40;
#endif
    alignas(void*) char m_storage[PTHREAD_MUTEX_SIZE];
};

template<typename RecursivePolicy, typename InterprocessPolicy>
class SYNC_API MutexBase : public PlatformMutex {
public:
    MutexBase();

    virtual ~MutexBase() = default;
};

using Mutex = MutexBase<PolicyNonRecursive, PolicyIntraprocess>;
using RecursiveMutex = MutexBase<PolicyRecursive, PolicyIntraprocess>;
using InterprocessMutex = MutexBase<PolicyNonRecursive, PolicyInterprocess>;
using InterprocessRecursiveMutex = MutexBase<PolicyRecursive, PolicyInterprocess>;

class [[nodiscard]] SYNC_API MutexLocker {
    AK_MAKE_NONCOPYABLE(MutexLocker);
    AK_MAKE_NONMOVABLE(MutexLocker);

public:
    ALWAYS_INLINE explicit MutexLocker(PlatformMutex& mutex)
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
    PlatformMutex& m_mutex;
};

}
