/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Page/Page.h>

namespace Web::Bindings {

void PrincipalHostDefined::visit_edges(JS::Cell::Visitor& visitor)
{
    JS::Realm::HostDefined::visit_edges(visitor);
    visitor.visit(environment_settings_object);
    visitor.visit(intrinsics);
    visitor.visit(page);
}

}
