/*
 * Copyright (c) 2026, Reimar Pihl Browa <mail@reim.ar>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/ExternalPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/External.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(External);

void External::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(External);
    Base::initialize(realm);
}

GC::Ref<External> External::create(JS::Realm& realm)
{
    return realm.create<External>(realm);
}

External::External(JS::Realm& realm)
    : PlatformObject(realm)
{
}

External::~External() = default;

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-external-addsearchprovider
void External::add_search_provider()
{
    // Do nothing
}

// https://html.spec.whatwg.org/multipage/obsolete.html#dom-external-issearchproviderinstalled
void External::is_search_provider_installed()
{
    // Do nothing
}

}
