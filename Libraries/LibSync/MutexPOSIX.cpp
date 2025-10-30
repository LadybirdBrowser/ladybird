/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, kleines Filmröllchen <malu.bertsch@gmail.com>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Concepts.h>
#include <AK/Diagnostics.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Platform.h>
#include <LibSync/Export.h>
#include <LibSync/Mutex.h>
#include <LibSync/Policy.h>
#include <pthread.h>

namespace Sync {

namespace {

ALWAYS_INLINE pthread_mutex_t* to_impl(void* ptr)
{
    return reinterpret_cast<pthread_mutex_t*>(ptr);
}

}

template<typename R, typename I>
MutexBase<R, I>::~MutexBase()
{
    int result = pthread_mutex_destroy(to_impl(m_storage));
    if (result != 0) {
        warnln("pthread_mutex_destroy failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

template<typename R, typename I>
bool MutexBase<R, I>::try_lock()
{
    int result = pthread_mutex_trylock(to_impl(m_storage));
    if (result == 0)
        return true;
    if (result == EBUSY)
        return false;
    warnln("pthread_mutex_lock failed with: {}", Error::from_errno(result));
    VERIFY_NOT_REACHED();
}

template<typename R, typename I>
void MutexBase<R, I>::lock()
{
    int result = pthread_mutex_lock(to_impl(m_storage));
    if (result != 0) {
        warnln("pthread_mutex_lock failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

template<typename R, typename I>
void MutexBase<R, I>::unlock()
{
    int result = pthread_mutex_unlock(to_impl(m_storage));
    if (result != 0) {
        warnln("pthread_mutex_unlock failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

template<typename RecursivePolicy, typename InterprocessPolicy>
MutexBase<RecursivePolicy, InterprocessPolicy>::MutexBase()
{
    static_assert(sizeof(m_storage) == sizeof(pthread_mutex_t));
    pthread_mutex_t* mutex_ptr = new (m_storage) pthread_mutex_t;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if constexpr (IsSame<RecursivePolicy, PolicyRecursive>) {
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    } else {
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    }
    if constexpr (IsSame<InterprocessPolicy, PolicyInterprocess>)
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    int result = pthread_mutex_init(mutex_ptr, &attr);
    if (result != 0) {
        warnln("pthread_mutex_init failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
    pthread_mutexattr_destroy(&attr);
}

template class SYNC_API MutexBase<PolicyNonRecursive, PolicyIntraprocess>;
template class SYNC_API MutexBase<PolicyRecursive, PolicyIntraprocess>;
template class SYNC_API MutexBase<PolicyNonRecursive, PolicyInterprocess>;
template class SYNC_API MutexBase<PolicyRecursive, PolicyInterprocess>;

}
