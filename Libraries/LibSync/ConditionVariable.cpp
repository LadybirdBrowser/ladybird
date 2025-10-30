/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>.
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSync/ConditionVariable.h>
#include <LibSync/Export.h>
#include <LibSync/Mutex.h>
#include <pthread.h>

namespace Sync {

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
ConditionVariableBase<MutexType>::ConditionVariableBase(MutexType& to_wait_on)
    : m_to_wait_on(to_wait_on)
{
    static_assert(sizeof(m_storage) == sizeof(pthread_cond_t));
    int result = pthread_cond_init(reinterpret_cast<pthread_cond_t*>(m_storage), nullptr);
    VERIFY(result == 0);
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
ConditionVariableBase<MutexType>::~ConditionVariableBase()
{
    int result = pthread_cond_destroy(reinterpret_cast<pthread_cond_t*>(m_storage));
    VERIFY(result == 0);
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
void ConditionVariableBase<MutexType>::wait()
{
    int result = pthread_cond_wait(reinterpret_cast<pthread_cond_t*>(m_storage), reinterpret_cast<pthread_mutex_t*>(m_to_wait_on.m_storage));
    VERIFY(result == 0);
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
void ConditionVariableBase<MutexType>::signal()
{
    int result = pthread_cond_signal(reinterpret_cast<pthread_cond_t*>(m_storage));
    VERIFY(result == 0);
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
void ConditionVariableBase<MutexType>::broadcast()
{
    int result = pthread_cond_broadcast(reinterpret_cast<pthread_cond_t*>(m_storage));
    VERIFY(result == 0);
}

template class SYNC_API ConditionVariableBase<Mutex>;

}
