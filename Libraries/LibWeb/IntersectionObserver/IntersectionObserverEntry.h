/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::IntersectionObserver {

class IntersectionObserverEntry final : public Bindings::Wrappable {
    WEB_WRAPPABLE(IntersectionObserverEntry, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(IntersectionObserverEntry);

public:
    static GC::Ref<IntersectionObserverEntry> create(HighResolutionTime::DOMHighResTimeStamp time, GC::Ptr<Geometry::DOMRectReadOnly> root_bounds, GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect, GC::Ref<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, GC::Ref<DOM::Element> target);

    virtual ~IntersectionObserverEntry() override;

    HighResolutionTime::DOMHighResTimeStamp time() const { return m_time; }
    GC::Ptr<Geometry::DOMRectReadOnly> root_bounds() const { return m_root_bounds; }
    GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect() const { return m_bounding_client_rect; }
    GC::Ref<Geometry::DOMRectReadOnly> intersection_rect() const { return m_intersection_rect; }
    bool is_intersecting() const { return m_is_intersecting; }
    double intersection_ratio() const { return m_intersection_ratio; }
    GC::Ref<DOM::Element> target() const { return m_target; }

private:
    IntersectionObserverEntry(HighResolutionTime::DOMHighResTimeStamp time, GC::Ptr<Geometry::DOMRectReadOnly> root_bounds, GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect, GC::Ref<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, GC::Ref<DOM::Element> target);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-time
    HighResolutionTime::DOMHighResTimeStamp m_time { 0.0 };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-rootbounds
    GC::Ptr<Geometry::DOMRectReadOnly> m_root_bounds;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-boundingclientrect
    GC::Ref<Geometry::DOMRectReadOnly> m_bounding_client_rect;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-intersectionrect
    GC::Ref<Geometry::DOMRectReadOnly> m_intersection_rect;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-isintersecting
    bool m_is_intersecting { false };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-intersectionratio
    double m_intersection_ratio { 0.0 };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-target
    GC::Ref<DOM::Element> m_target;
};

}
