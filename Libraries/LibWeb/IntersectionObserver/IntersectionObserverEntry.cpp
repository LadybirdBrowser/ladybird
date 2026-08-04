/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/IntersectionObserver/IntersectionObserverEntry.h>

namespace Web::IntersectionObserver {

GC_DEFINE_ALLOCATOR(IntersectionObserverEntry);

GC::Ref<IntersectionObserverEntry> IntersectionObserverEntry::create(HighResolutionTime::DOMHighResTimeStamp time, GC::Ptr<Geometry::DOMRectReadOnly> root_bounds, GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect, GC::Ref<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, GC::Ref<DOM::Element> target)
{
    return GC::Heap::the().allocate<IntersectionObserverEntry>(time, root_bounds, bounding_client_rect, intersection_rect, is_intersecting, intersection_ratio, target);
}

IntersectionObserverEntry::IntersectionObserverEntry(HighResolutionTime::DOMHighResTimeStamp time, GC::Ptr<Geometry::DOMRectReadOnly> root_bounds, GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect, GC::Ref<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, GC::Ref<DOM::Element> target)
    : m_time(time)
    , m_root_bounds(root_bounds)
    , m_bounding_client_rect(bounding_client_rect)
    , m_intersection_rect(intersection_rect)
    , m_is_intersecting(is_intersecting)
    , m_intersection_ratio(intersection_ratio)
    , m_target(target)
{
}

IntersectionObserverEntry::~IntersectionObserverEntry() = default;

void IntersectionObserverEntry::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_root_bounds);
    visitor.visit(m_bounding_client_rect);
    visitor.visit(m_intersection_rect);
    visitor.visit(m_target);
}

}
