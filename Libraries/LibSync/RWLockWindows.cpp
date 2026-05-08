/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Windows.h>
#include <LibSync/Export.h>
#include <LibSync/RWLock.h>

namespace Sync {

namespace {

ALWAYS_INLINE PSRWLOCK to_impl(void* ptr)
{
    return reinterpret_cast<PSRWLOCK>(ptr);
}

}

RWLock::RWLock()
{
    static_assert(sizeof(SRWLOCK) == sizeof(m_storage));
    PSRWLOCK rwlock_ptr = new (m_storage) SRWLOCK;
    InitializeSRWLock(rwlock_ptr);
}

RWLock::~RWLock() = default;

bool RWLock::try_lock_read()
{
    return TryAcquireSRWLockShared(to_impl(m_storage));
}

bool RWLock::try_lock_write()
{
    return TryAcquireSRWLockExclusive(to_impl(m_storage));
}

void RWLock::lock_read()
{
    AcquireSRWLockShared(to_impl(m_storage));
}

void RWLock::lock_write()
{
    AcquireSRWLockExclusive(to_impl(m_storage));
}

void RWLock::unlock_read()
{
    ReleaseSRWLockShared(to_impl(m_storage));
}

void RWLock::unlock_write()
{
    ReleaseSRWLockExclusive(to_impl(m_storage));
}

}
