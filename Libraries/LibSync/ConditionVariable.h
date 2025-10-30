/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>.
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <LibSync/Export.h>
#include <LibSync/Mutex.h>
#include <LibSync/Policy.h>

#if !defined(AK_OS_WINDOWS)
#    include <pthread.h>
#endif

namespace Sync {

// A signaling condition variable that wraps over the platform APIs.
// On posix it is a wrapper of pthread_cond_*.
// On Windows it wraps ConditionVariable
// TODO: Implement timed_wait()
template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
class SYNC_API ConditionVariableBase {
    AK_MAKE_NONCOPYABLE(ConditionVariableBase);
    AK_MAKE_NONMOVABLE(ConditionVariableBase);

public:
    ConditionVariableBase(MutexType& to_wait_on);
    ~ConditionVariableBase();

    // As with pthread APIs, the mutex must be locked or undefined behavior ensues.
    // Condition variables are allowed spurious wakeups. As such waiting on a condition in a loop is preferred.
    void wait();

    ALWAYS_INLINE void wait_while(Function<bool()> condition)
    {
        while (condition())
            wait();
    }
    // Release at least one of the threads waiting on this variable.
    void signal();

    // Release all of the threads waiting on this variable.
    void broadcast();

private:
#ifdef AK_OS_WINDOWS
    using StorageType = void*;
#else
    using StorageType = pthread_cond_t;
#endif

    alignas(StorageType) unsigned char m_storage[sizeof(StorageType)];
    MutexType& m_to_wait_on;
};

template<typename MutexType>
ConditionVariableBase(MutexType&) -> ConditionVariableBase<MutexType>;

using ConditionVariable = ConditionVariableBase<Mutex>;

}
