/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::IntersectionObserver {

struct IntersectionObserverEntryInit {
    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-time
    HighResolutionTime::DOMHighResTimeStamp time { 0.0 };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-rootbounds
    Optional<Geometry::DOMRectInit> root_bounds;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-boundingclientrect
    Geometry::DOMRectInit bounding_client_rect;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-intersectionrect
    Geometry::DOMRectInit intersection_rect;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-isintersecting
    bool is_intersecting { false };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-intersectionratio
    double intersection_ratio { 0.0 };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-target
    GC::Root<DOM::Element> target;
};

class IntersectionObserverEntry final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IntersectionObserverEntry, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IntersectionObserverEntry);

public:
    static WebIDL::ExceptionOr<GC::Ref<IntersectionObserverEntry>> construct_impl(JS::Realm&, IntersectionObserverEntryInit const& options);

    virtual ~IntersectionObserverEntry() override;

    HighResolutionTime::DOMHighResTimeStamp time() const { return m_time; }
    GC::Ptr<Geometry::DOMRectReadOnly> root_bounds() const { return m_root_bounds; }
    GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect() const { return m_bounding_client_rect; }
    GC::Ref<Geometry::DOMRectReadOnly> intersection_rect() const { return m_intersection_rect; }
    bool is_intersecting() const { return m_is_intersecting; }
    double intersection_ratio() const { return m_intersection_ratio; }
    GC::Ref<DOM::Element> target() const { return m_target; }

private:
    IntersectionObserverEntry(JS::Realm&, HighResolutionTime::DOMHighResTimeStamp time, GC::Ptr<Geometry::DOMRectReadOnly> root_bounds, GC::Ref<Geometry::DOMRectReadOnly> bounding_client_rect, GC::Ref<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, GC::Ref<DOM::Element> target);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

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
