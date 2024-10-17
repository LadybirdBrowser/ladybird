/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NavigationActivationPrototype.h>
#include <LibWeb/HTML/NavigationActivation.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(NavigationActivation);

NavigationActivation::NavigationActivation(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

NavigationActivation::~NavigationActivation() = default;

void NavigationActivation::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(NavigationActivation);
}

void NavigationActivation::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_old_entry);
    visitor.visit(m_new_entry);
}

}
