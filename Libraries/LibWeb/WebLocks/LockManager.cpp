/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebLocks/LockManager.h>

namespace Web::WebLocks {

extern "C" {
void* web_rust_web_locks_create_lock_manager(JSVM*, void* realm);
LockManager* web_locks_environment_lock_manager(HTML::EnvironmentSettingsObject*);
}

GC::Ref<LockManager> LockManager::create(HTML::EnvironmentSettingsObject& environment)
{
    return *static_cast<LockManager*>(web_rust_web_locks_create_lock_manager(JS::HostABI::vm_to_abi(environment.realm().vm()), &environment.realm()));
}

LockManager* web_locks_environment_lock_manager(HTML::EnvironmentSettingsObject* environment)
{
    return environment->lock_manager().ptr();
}

}
