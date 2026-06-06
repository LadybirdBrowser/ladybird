/*
 * Copyright (c) 2026, Reimar Pihl Browa <mail@reim.ar>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/External.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(External);

GC::Ref<External> External::create(JS::Realm& realm)
{
    return realm.create<External>(realm);
}

External::External(JS::Realm& realm)
    : Wrappable(realm)
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
