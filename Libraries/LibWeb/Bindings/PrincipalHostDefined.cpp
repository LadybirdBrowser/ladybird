/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Page/Page.h>

namespace Web::Bindings {

OwnPtr<JS::Realm::HostDefined> create_principal_host_defined(GC::Ref<HTML::EnvironmentSettingsObject> environment_settings_object, GC::Ref<Intrinsics> intrinsics, GC::Ref<Page> page)
{
    auto wrapper_world = GC::Heap::the().allocate<WrapperWorld>(WrapperWorld::Type::Main);
    return make<PrincipalHostDefined>(environment_settings_object, intrinsics, *wrapper_world, page);
}

void PrincipalHostDefined::visit_edges(JS::Cell::Visitor& visitor)
{
    HostDefined::visit_edges(visitor);
    visitor.visit(environment_settings_object);
    visitor.visit(page);
}

}
