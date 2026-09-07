/*
 * Copyright (c) 2024, Colin Reeder <colin@vpzom.click>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/NavigationTiming/PerformanceNavigation.h>
#include <LibWeb/NavigationTiming/PerformanceNavigationTiming.h>

namespace Web::NavigationTiming {

GC_DEFINE_ALLOCATOR(PerformanceNavigation);

GC::Ref<PerformanceNavigation> PerformanceNavigation::create(HTML::Window& window)
{
    return GC::Heap::the().allocate<PerformanceNavigation>(window);
}

PerformanceNavigation::PerformanceNavigation(HTML::Window& window)
    : m_window(window)
{
}

PerformanceNavigation::~PerformanceNavigation() = default;

void PerformanceNavigation::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

GC::Ptr<PerformanceNavigationTiming> PerformanceNavigation::navigation_timing_entry() const
{
    return m_window->associated_document().navigation_timing_entry();
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigation-type
u16 PerformanceNavigation::type() const
{
    // This attribute must return the type of the last non-redirect navigation. It must have one of the following
    // navigation type values.
    auto entry = navigation_timing_entry();
    if (!entry)
        return TYPE_NAVIGATE;

    switch (entry->type()) {
    case Bindings::NavigationTimingType::Navigate:
        return TYPE_NAVIGATE;
    case Bindings::NavigationTimingType::Reload:
        return TYPE_RELOAD;
    case Bindings::NavigationTimingType::BackForward:
        return TYPE_BACK_FORWARD;
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/navigation-timing/#dom-performancenavigation-redirectcount
u16 PerformanceNavigation::redirect_count() const
{
    // This attribute must return the number of redirects since the last non-redirect navigation. If there is no
    // redirect or there is any redirect that is not from the same origin as the destination document, this attribute
    // must return zero.
    auto entry = navigation_timing_entry();
    if (!entry)
        return 0;

    return entry->redirect_count();
}

}
