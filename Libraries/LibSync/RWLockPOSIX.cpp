/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Platform.h>
#include <LibSync/Export.h>
#include <LibSync/RWLock.h>
#include <new>
#include <pthread.h>

namespace Sync {

namespace {

ALWAYS_INLINE pthread_rwlock_t* to_impl(void* ptr)
{
    return reinterpret_cast<pthread_rwlock_t*>(ptr);
}

}

RWLock::RWLock()
{
    static_assert(sizeof(pthread_rwlock_t) == sizeof(m_storage));
    auto* rwlock_ptr = new (m_storage) pthread_rwlock_t;
    int result = pthread_rwlock_init(rwlock_ptr, nullptr);
    if (result != 0) {
        warnln("pthread_rwlock_unlock failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

RWLock::~RWLock()
{
    int result = pthread_rwlock_destroy(to_impl(m_storage));
    if (result != 0) {
        warnln("pthread_rwlock_destroy failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

bool RWLock::try_lock_read()
{
    int result = pthread_rwlock_tryrdlock(to_impl(m_storage));
    if (result == 0)
        return true;
    if (result == EBUSY)
        return false;
    warnln("pthread_rwlock_trywrlock failed with: {}", Error::from_errno(result));
    VERIFY_NOT_REACHED();
}

bool RWLock::try_lock_write()
{
    int result = pthread_rwlock_trywrlock(to_impl(m_storage));
    if (result == 0)
        return true;
    if (result == EBUSY)
        return false;
    warnln("pthread_rwlock_trywrlock failed with: {}", Error::from_errno(result));
    VERIFY_NOT_REACHED();
}
void RWLock::lock_read()
{
    int result = pthread_rwlock_rdlock(to_impl(m_storage));
    if (result != 0) {
        warnln("pthread_rwlock_rdlock failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

void RWLock::lock_write()
{
    int result = pthread_rwlock_wrlock(to_impl(m_storage));
    if (result != 0) {
        warnln("pthread_rwlock_wrlock failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

void RWLock::unlock_read()
{
    int result = pthread_rwlock_unlock(to_impl(m_storage));
    if (result != 0) {
        warnln("pthread_rwlock_unlock failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

void RWLock::unlock_write()
{
    int result = pthread_rwlock_unlock(to_impl(m_storage));
    if (result != 0) {
        warnln("pthread_rwlock_unlock failed with: {}", Error::from_errno(result));
        VERIFY_NOT_REACHED();
    }
}

}
