/*
 * Copyright (c) 2024, Ali Mohammad Pur <mpfard@serenityos.org>
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
#include <LibSync/Policy.h>
#include <LibSync/RWLock.h>
#include <new>
#include <pthread.h>

namespace Sync {

namespace Detail {

struct RWLockImpl {
    pthread_rwlock_t lock;
};

}

PlatformRWLock::PlatformRWLock()
{
    static_assert(sizeof(Detail::RWLockImpl) <= sizeof(m_storage));
#ifdef AK_COMPILER_GCC
    static_assert(alignof(Detail::RWLockImpl) <= alignof(m_storage));
#else
    AK_IGNORE_DIAGNOSTIC("-Wgnu-alignof-expression",
        static_assert(alignof(Detail::RWLockImpl) <= alignof(m_storage));)
#endif
    new (&impl()) Detail::RWLockImpl;
}

PlatformRWLock::~PlatformRWLock()
{
    int rc = pthread_rwlock_destroy(&impl().lock);
    if (rc != 0) {
        warnln("pthread_rwlock_destroy failed with: {}", Error::from_errno(rc));
        VERIFY_NOT_REACHED();
    }
}

bool PlatformRWLock::try_lock_read()
{
    int rc = pthread_rwlock_tryrdlock(&impl().lock);
    return rc == 0;
}

bool PlatformRWLock::try_lock_write()
{
    int rc = pthread_rwlock_trywrlock(&impl().lock);
    return rc == 0;
}
void PlatformRWLock::lock_read()
{
    int rc = pthread_rwlock_rdlock(&impl().lock);
    if (rc != 0) {
        warnln("pthread_rwlock_rdlock failed with: {}", Error::from_errno(rc));
        VERIFY_NOT_REACHED();
    }
}

void PlatformRWLock::lock_write()
{
    int rc = pthread_rwlock_wrlock(&impl().lock);
    if (rc != 0) {
        warnln("pthread_rwlock_wrlock failed with: {}", Error::from_errno(rc));
        VERIFY_NOT_REACHED();
    }
}

void PlatformRWLock::unlock()
{
    int rc = pthread_rwlock_unlock(&impl().lock);
    if (rc != 0) {
        warnln("pthread_rwlock_unlock failed with: {}", Error::from_errno(rc));
        VERIFY_NOT_REACHED();
    }
}

template<typename InterprocessPolicy>
RWLockBase<InterprocessPolicy>::RWLockBase()
{
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    // FIXME: Once this file isn't used on windows deduplicate the code.
    // It is like this as pthread-win32 fails with EINVAL on a nonnull attr.
    int rc;
    if constexpr (IsSame<InterprocessPolicy, PolicyInterprocess>) {
        pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        rc = pthread_rwlock_init(&impl().lock, &attr);
    } else {
        rc = pthread_rwlock_init(&impl().lock, nullptr);
    }
    if (rc != 0) {
        warnln("pthread_rwlock_init failed with: {}", Error::from_errno(rc));
        VERIFY_NOT_REACHED();
    }
    pthread_rwlockattr_destroy(&attr);
}

template class SYNC_API RWLockBase<PolicyIntraprocess>;
template class SYNC_API RWLockBase<PolicyInterprocess>;

}
