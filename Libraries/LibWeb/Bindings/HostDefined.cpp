/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebAssembly/WebAssembly.h>

namespace Web::Bindings {

HostDefined::HostDefined(GC::Ref<Intrinsics> intrinsics, GC::Ref<WrapperWorld> wrapper_world, GC::Ref<JS::Realm> principal_realm, PrincipalRealmUnderConstruction principal_realm_under_construction)
    : intrinsics(intrinsics)
    , wrapper_world(wrapper_world)
    , principal_realm(principal_realm)
{
    if (principal_realm_under_construction == PrincipalRealmUnderConstruction::No) {
        VERIFY(principal_realm->host_defined());
        VERIFY(principal_realm->host_defined()->is_principal_host_defined());
    }
}

HostDefined::~HostDefined() = default;

void HostDefined::visit_edges(JS::Cell::Visitor& visitor)
{
    JS::Realm::HostDefined::visit_edges(visitor);
    visitor.visit(intrinsics);
    visitor.visit(wrapper_world);
    visitor.visit(principal_realm);
    if (wasm_cache)
        wasm_cache->visit_edges(visitor);
}

}
