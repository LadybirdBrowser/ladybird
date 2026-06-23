/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebLocks/LockManager.h>
#include <LibWeb/WebLocks/NavigatorLocks.h>

namespace Web::WebLocks {

// https://w3c.github.io/web-locks/#dom-navigatorlocks-locks
GC::Ref<LockManager> NavigatorLocks::locks()
{
    // The locks getter’s steps are to return this’s relevant settings object’s LockManager object.
    return HTML::relevant_settings_object(this_navigator_locks_object()).lock_manager();
}

}
