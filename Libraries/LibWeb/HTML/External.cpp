/*
 * Copyright (c) 2026, Reimar Pihl Browa <mail@reim.ar>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/External.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(External);

GC::Ref<External> External::create()
{
    return GC::Heap::the().allocate<External>();
}

External::External()
    : Bindings::Wrappable()
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
