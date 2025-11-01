/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, kleines Filmröllchen <malu.bertsch@gmail.com>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Concepts.h>
#include <AK/Diagnostics.h>
#include <AK/Platform.h>
#include <LibSync/Export.h>
#include <LibSync/Mutex.h>
#include <LibSync/Policy.h>
#include <new>
#include <pthread.h>

namespace Sync {

namespace Detail {

struct MutexImpl {
    pthread_mutex_t mutex;
};

}

PlatformMutex::PlatformMutex()
{
    static_assert(sizeof(Detail::MutexImpl) <= sizeof(m_storage));
#ifdef AK_COMPILER_GCC
    static_assert(alignof(Detail::MutexImpl) <= alignof(m_storage));
#else
    AK_IGNORE_DIAGNOSTIC("-Wgnu-alignof-expression",
        static_assert(alignof(Detail::MutexImpl) <= alignof(m_storage));)
#endif
    new (&impl()) Detail::MutexImpl;
}

PlatformMutex::~PlatformMutex()
{
    pthread_mutex_destroy(&impl().mutex);
}

void PlatformMutex::lock()
{
    pthread_mutex_lock(&impl().mutex);
}

void PlatformMutex::unlock()
{
    pthread_mutex_unlock(&impl().mutex);
}

template<typename RecursivePolicy, typename InterprocessPolicy>
MutexBase<RecursivePolicy, InterprocessPolicy>::MutexBase()
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if constexpr (IsSame<RecursivePolicy, PolicyRecursive>)
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if constexpr (IsSame<InterprocessPolicy, PolicyInterprocess>)
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&impl().mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

template class SYNC_API MutexBase<PolicyNonRecursive, PolicyIntraprocess>;
template class SYNC_API MutexBase<PolicyRecursive, PolicyIntraprocess>;
template class SYNC_API MutexBase<PolicyNonRecursive, PolicyInterprocess>;
template class SYNC_API MutexBase<PolicyRecursive, PolicyInterprocess>;

}
