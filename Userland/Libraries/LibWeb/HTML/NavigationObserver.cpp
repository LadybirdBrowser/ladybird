/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationObserver.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(NavigationObserver);

NavigationObserver::NavigationObserver(JS::Realm& realm, Navigable& navigable)
    : Bindings::PlatformObject(realm)
    , m_navigable(navigable)
{
    m_navigable->register_navigation_observer({}, *this);
}

void NavigationObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_navigable);
    visitor.visit(m_navigation_complete);
}

void NavigationObserver::finalize()
{
    Base::finalize();
    m_navigable->unregister_navigation_observer({}, *this);
}

void NavigationObserver::set_navigation_complete(Function<void()> callback)
{
    if (callback)
        m_navigation_complete = JS::create_heap_function(vm().heap(), move(callback));
    else
        m_navigation_complete = nullptr;
}

}
