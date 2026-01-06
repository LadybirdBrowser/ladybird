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

RWLock::RWLock()
{
    static_assert(sizeof(SRWLOCK) == sizeof(m_storage));
    PSRWLOCK rwlock_ptr = new (m_storage) SRWLOCK;
    InitializeSRWLock(rwlock_ptr);
}

RWLock::~RWLock() = default;

bool RWLock::try_lock_read()
{
    return TryAcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(m_storage));
}

bool RWLock::try_lock_write()
{
    return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(m_storage));
}

void RWLock::lock_read()
{
    AcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(m_storage));
}

void RWLock::lock_write()
{
    AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(m_storage));
}

void RWLock::unlock_read()
{
    ReleaseSRWLockShared(reinterpret_cast<PSRWLOCK>(m_storage));
}

void RWLock::unlock_write()
{
    ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(m_storage));
}

}
