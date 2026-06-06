/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationObserver.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(NavigationObserver);

NavigationObserver::NavigationObserver(Navigable& navigable)
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

void NavigationObserver::set_navigation_complete(Function<void()> callback)
{
    if (callback)
        m_navigation_complete = GC::create_function(heap(), move(callback));
    else
        m_navigation_complete = nullptr;
}

void NavigationObserver::set_ongoing_navigation_changed(Function<void()> callback)
{
    if (callback)
        m_ongoing_navigation_changed = GC::create_function(heap(), move(callback));
    else
        m_ongoing_navigation_changed = nullptr;
}

}
