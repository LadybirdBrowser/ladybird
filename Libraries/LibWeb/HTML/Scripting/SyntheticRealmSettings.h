/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://whatpr.org/html/9893/webappapis.html#synthetic-realm-settings-objects
// Each synthetic realm has an associated synthetic realm settings object with the following fields:
struct SyntheticRealmSettings {
    // An execution context
    // The JavaScript execution context for the scripts within this realm.
    NonnullOwnPtr<JS::ExecutionContext> execution_context;

    // A principal realm
    // The principal realm which this synthetic realm exists within.
    GC::Ref<JS::Realm> principal_realm;

    // An underlying realm
    // The synthetic realm which this settings object represents.
    GC::Ref<JS::Realm> underlying_realm;

    // A module map
    // A module map that is used when importing JavaScript modules.
    GC::Ref<ModuleMap> module_map;

    void visit_edges(JS::Cell::Visitor&);
};

}
