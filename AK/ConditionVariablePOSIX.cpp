/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>.
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/Platform.h>
#include <AK/Time.h>
#include <errno.h>
#include <pthread.h>

namespace AK {

namespace {

ALWAYS_INLINE pthread_cond_t* to_impl(void* ptr)
{
    return reinterpret_cast<pthread_cond_t*>(ptr);
}

}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
ConditionVariableBase<MutexType>::ConditionVariableBase(MutexType& to_wait_on)
    : m_to_wait_on(to_wait_on)
{
    static_assert(sizeof(m_storage) == sizeof(pthread_cond_t));
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    int result = pthread_cond_init(to_impl(m_storage), nullptr);
    VERIFY(result == 0);
#else
    pthread_condattr_t attributes;
    int result = pthread_condattr_init(&attributes);
    VERIFY(result == 0);
    result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    VERIFY(result == 0);
    result = pthread_cond_init(to_impl(m_storage), &attributes);
    VERIFY(result == 0);
    result = pthread_condattr_destroy(&attributes);
    VERIFY(result == 0);
#endif
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
ConditionVariableBase<MutexType>::~ConditionVariableBase()
{
    int result = pthread_cond_destroy(to_impl(m_storage));
    VERIFY(result == 0);
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
void ConditionVariableBase<MutexType>::wait()
{
    int result = pthread_cond_wait(to_impl(m_storage), reinterpret_cast<pthread_mutex_t*>(m_to_wait_on.m_storage));
    VERIFY(result == 0);
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
bool ConditionVariableBase<MutexType>::wait_for(AK::Duration const& timeout)
{
    if (timeout <= AK::Duration::zero())
        return false;

    auto* mutex = reinterpret_cast<pthread_mutex_t*>(m_to_wait_on.m_storage);
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    auto relative_timeout = timeout.to_timespec();
    int result = pthread_cond_timedwait_relative_np(to_impl(m_storage), mutex, &relative_timeout);
#else
    auto absolute_timeout = (MonotonicTime::now() + timeout).to_timespec();
    int result = pthread_cond_timedwait(to_impl(m_storage), mutex, &absolute_timeout);
#endif
    if (result == ETIMEDOUT)
        return false;
    VERIFY(result == 0);
    return true;
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
void ConditionVariableBase<MutexType>::signal()
{
    int result = pthread_cond_signal(to_impl(m_storage));
    VERIFY(result == 0);
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
void ConditionVariableBase<MutexType>::broadcast()
{
    int result = pthread_cond_broadcast(to_impl(m_storage));
    VERIFY(result == 0);
}

template class ConditionVariableBase<Mutex>;

}
