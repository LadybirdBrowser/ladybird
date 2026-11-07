/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigationObserver.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(NavigationObserver);

GC::Ref<NavigationObserver> NavigationObserver::create(LocalNavigable& navigable)
{
    return GC::Heap::the().allocate<NavigationObserver>(navigable);
}

NavigationObserver::NavigationObserver(LocalNavigable& navigable)
    : m_navigable(navigable)
{
    m_navigable->register_navigation_observer({}, *this);
}

void NavigationObserver::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_navigable);
    visitor.visit(m_navigation_complete);
    visitor.visit(m_ongoing_navigation_changed);
}

void NavigationObserver::finalize()
{
    Base::finalize();
    m_navigable->unregister_navigation_observer({}, *this);
}

void NavigationObserver::set_navigation_complete(GC::Ptr<GC::Function<void()>> callback)
{
    m_navigation_complete = callback;
}

void NavigationObserver::set_ongoing_navigation_changed(GC::Ptr<GC::Function<void()>> callback)
{
    m_ongoing_navigation_changed = callback;
}

}
