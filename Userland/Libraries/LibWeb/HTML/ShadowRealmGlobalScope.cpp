/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ShadowRealmExposedInterfaces.h>
#include <LibWeb/Bindings/ShadowRealmGlobalScopePrototype.h>
#include <LibWeb/HTML/ShadowRealmGlobalScope.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(ShadowRealmGlobalScope);

ShadowRealmGlobalScope::ShadowRealmGlobalScope(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

ShadowRealmGlobalScope::~ShadowRealmGlobalScope() = default;

JS::NonnullGCPtr<ShadowRealmGlobalScope> ShadowRealmGlobalScope::create(JS::Realm& realm)
{
    return realm.heap().allocate<ShadowRealmGlobalScope>(realm, realm);
}

void ShadowRealmGlobalScope::initialize(JS::Realm&)
{
}

void ShadowRealmGlobalScope::initialize_web_interfaces()
{
    auto& realm = this->realm();

    WEB_SET_PROTOTYPE_FOR_INTERFACE(ShadowRealmGlobalScope);

    add_shadow_realm_exposed_interfaces(*this);
    Bindings::ShadowRealmGlobalScopeGlobalMixin::initialize(realm, *this);
}

void ShadowRealmGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
