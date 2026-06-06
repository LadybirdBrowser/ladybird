/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/NavigationActivation.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(NavigationActivation);

GC::Ref<NavigationActivation> NavigationActivation::create(GC::Ptr<NavigationHistoryEntry> from, GC::Ref<NavigationHistoryEntry> entry, Bindings::NavigationType navigation_type)
{
    return GC::Heap::the().allocate<NavigationActivation>(from, entry, navigation_type);
}

NavigationActivation::NavigationActivation(GC::Ptr<NavigationHistoryEntry> from, GC::Ref<NavigationHistoryEntry> entry, Bindings::NavigationType navigation_type)
    : Bindings::Wrappable()
    , m_from(from)
    , m_entry(entry)
    , m_navigation_type(navigation_type)
{
}

NavigationActivation::~NavigationActivation() = default;

void NavigationActivation::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_from);
    visitor.visit(m_entry);
}

}
