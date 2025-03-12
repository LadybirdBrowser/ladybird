/*
 * Copyright (c) 2025, Bogi Napoleon Wennerstrøm <bogi.wennerstrom@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebLocks/LockManager.h>

namespace Web::HTML {
class NavigatorLocksMixin {
public:
    GC::Ref<WebLocks::LockManager> locks() const;

    friend class Navigator;
    friend class WorkerNavigator;

protected:
    virtual Bindings::PlatformObject const& this_navigator_locks_object() const = 0;

private:
    mutable GC::Ptr<WebLocks::LockManager> m_locks;
};
}
