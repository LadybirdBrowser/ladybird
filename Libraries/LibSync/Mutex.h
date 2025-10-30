/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, kleines Filmröllchen <malu.bertsch@gmail.com>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Concepts.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <LibSync/Export.h>
#include <LibSync/Policy.h>

#if !defined(AK_OS_WINDOWS)
#    include <pthread.h>
#endif

namespace Sync {

template<typename RecursivePolicy, typename InterprocessPolicy>
class MutexBase;

template<typename T>
requires Detail::IsIntraprocess<T> && Detail::IsNonRecursive<T>
class ConditionVariableBase;

template<typename RecursivePolicy, typename InterprocessPolicy>
class SYNC_API MutexBase {
    AK_MAKE_NONCOPYABLE(MutexBase);
    AK_MAKE_NONMOVABLE(MutexBase);

    template<typename T>
    requires Detail::IsIntraprocess<T> && Detail::IsNonRecursive<T>
    friend class ConditionVariableBase;

public:
    using InterprocessPolicyType = InterprocessPolicy;
    using RecursivePolicyType = RecursivePolicy;

    MutexBase();
    ~MutexBase();

    bool try_lock();

    void lock();
    void unlock();

private:
    static consteval u64 storage_size()
    {
#ifdef AK_OS_WINDOWS
        if constexpr (IsSame<InterprocessPolicy, PolicyInterprocess>) {
            // Size of a handle
            return sizeof(void*);
        }
        if constexpr (IsSame<RecursivePolicy, PolicyRecursive>) {
            // The size of a critical section. This is guaranteed
            return 40;
        }
        // SRWLock is just a void*
        return sizeof(void*);
#else
        return sizeof(pthread_mutex_t);
#endif
    }

    alignas(void*) unsigned char m_storage[storage_size()];
};

using Mutex = MutexBase<PolicyNonRecursive, PolicyIntraprocess>;
using RecursiveMutex = MutexBase<PolicyRecursive, PolicyIntraprocess>;
using IPCMutex = MutexBase<PolicyNonRecursive, PolicyInterprocess>;
using IPCRecursiveMutex = MutexBase<PolicyRecursive, PolicyInterprocess>;

template<typename MutexType>
class [[nodiscard]] SYNC_API MutexLocker {
    AK_MAKE_NONCOPYABLE(MutexLocker);
    AK_MAKE_NONMOVABLE(MutexLocker);

public:
    ALWAYS_INLINE explicit MutexLocker(MutexType& mutex)
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
    MutexType& m_mutex;
};

template<typename MutexType>
MutexLocker(MutexType&) -> MutexLocker<MutexType>;

}
