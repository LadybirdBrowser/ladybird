/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/SyntheticHostDefined.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::Bindings {

void SyntheticHostDefined::visit_edges(JS::Cell::Visitor& visitor)
{
    JS::Realm::HostDefined::visit_edges(visitor);
    synthetic_realm_settings.visit_edges(visitor);
}

}
