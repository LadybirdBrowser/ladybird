/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/ResizeObserverPrototype.h>
#include <LibWeb/ResizeObserver/ResizeObserverSize.h>

namespace Web::ResizeObserver {

// https://drafts.csswg.org/resize-observer-1/#resize-observation-interface
class ResizeObservation : public JS::Cell {
    JS_CELL(ResizeObservation, JS::Cell);
    JS_DECLARE_ALLOCATOR(ResizeObservation);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<ResizeObservation>> create(JS::Realm&, DOM::Element&, Bindings::ResizeObserverBoxOptions);

    bool is_active();

    JS::NonnullGCPtr<DOM::Element> target() const { return m_target; }
    Bindings::ResizeObserverBoxOptions observed_box() const { return m_observed_box; }

    Vector<JS::NonnullGCPtr<ResizeObserverSize>>& last_reported_sizes() { return m_last_reported_sizes; }

    explicit ResizeObservation(JS::Realm& realm, DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box);

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    JS::NonnullGCPtr<JS::Realm> m_realm;
    JS::NonnullGCPtr<DOM::Element> m_target;
    Bindings::ResizeObserverBoxOptions m_observed_box;
    Vector<JS::NonnullGCPtr<ResizeObserverSize>> m_last_reported_sizes;
};

}
