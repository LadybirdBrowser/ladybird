/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Windows.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>

namespace Sync {

namespace {

ALWAYS_INLINE PCONDITION_VARIABLE to_impl(void* ptr)
{
    return reinterpret_cast<PCONDITION_VARIABLE>(ptr);
}

}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
ConditionVariableBase<MutexType>::ConditionVariableBase(MutexType& to_wait_on)
    : m_to_wait_on(to_wait_on)
{
    InitializeConditionVariable(to_impl(m_storage));
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
ConditionVariableBase<MutexType>::~ConditionVariableBase() = default;

template<>
void ConditionVariableBase<Mutex>::wait()
{
    BOOL result = SleepConditionVariableSRW(to_impl(m_storage), reinterpret_cast<PSRWLOCK>(m_to_wait_on.m_storage), INFINITE, 0);
    if (!result) {
        warnln("SleepConditionVariableSRW failed with: {}", Error::from_windows_error());
        VERIFY_NOT_REACHED();
    }
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
void ConditionVariableBase<MutexType>::signal()
{
    WakeConditionVariable(to_impl(m_storage));
}

template<typename MutexType>
requires Detail::IsIntraprocess<MutexType> && Detail::IsNonRecursive<MutexType>
void ConditionVariableBase<MutexType>::broadcast()
{
    WakeAllConditionVariable(to_impl(m_storage));
}

template class SYNC_API ConditionVariableBase<Mutex>;

}
