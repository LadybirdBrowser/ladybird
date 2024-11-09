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
    JS::Handle<DOM::Element> target;
};

class IntersectionObserverEntry final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IntersectionObserverEntry, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(IntersectionObserverEntry);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<IntersectionObserverEntry>> construct_impl(JS::Realm&, IntersectionObserverEntryInit const& options);

    virtual ~IntersectionObserverEntry() override;

    HighResolutionTime::DOMHighResTimeStamp time() const { return m_time; }
    JS::GCPtr<Geometry::DOMRectReadOnly> root_bounds() const { return m_root_bounds; }
    JS::NonnullGCPtr<Geometry::DOMRectReadOnly> bounding_client_rect() const { return m_bounding_client_rect; }
    JS::NonnullGCPtr<Geometry::DOMRectReadOnly> intersection_rect() const { return m_intersection_rect; }
    bool is_intersecting() const { return m_is_intersecting; }
    double intersection_ratio() const { return m_intersection_ratio; }
    JS::NonnullGCPtr<DOM::Element> target() const { return m_target; }

private:
    IntersectionObserverEntry(JS::Realm&, HighResolutionTime::DOMHighResTimeStamp time, JS::GCPtr<Geometry::DOMRectReadOnly> root_bounds, JS::NonnullGCPtr<Geometry::DOMRectReadOnly> bounding_client_rect, JS::NonnullGCPtr<Geometry::DOMRectReadOnly> intersection_rect, bool is_intersecting, double intersection_ratio, JS::NonnullGCPtr<DOM::Element> target);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-time
    HighResolutionTime::DOMHighResTimeStamp m_time { 0.0 };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-rootbounds
    JS::GCPtr<Geometry::DOMRectReadOnly> m_root_bounds;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-boundingclientrect
    JS::NonnullGCPtr<Geometry::DOMRectReadOnly> m_bounding_client_rect;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-intersectionrect
    JS::NonnullGCPtr<Geometry::DOMRectReadOnly> m_intersection_rect;

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-isintersecting
    bool m_is_intersecting { false };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-intersectionratio
    double m_intersection_ratio { 0.0 };

    // https://www.w3.org/TR/intersection-observer/#dom-intersectionobserverentry-target
    JS::NonnullGCPtr<DOM::Element> m_target;
};

}
