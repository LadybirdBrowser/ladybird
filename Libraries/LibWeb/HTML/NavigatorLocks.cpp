/*
 * Copyright (c) 2025, Bogi Napoleon Wennerstrøm <bogi.wennerstrom@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NavigatorLocks.h"

#include <LibWeb/WebLocks/LockManager.h>

namespace Web::HTML {

GC::Ref<WebLocks::LockManager> NavigatorLocksMixin::locks() const
{
    if (!m_locks) {
        auto& realm = this_navigator_locks_object().realm();
        m_locks = realm.create<WebLocks::LockManager>(realm);
    }

    return *m_locks;
}

}
