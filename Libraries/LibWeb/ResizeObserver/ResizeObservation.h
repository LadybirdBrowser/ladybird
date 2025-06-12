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
    GC_CELL(ResizeObservation, JS::Cell);
    GC_DECLARE_ALLOCATOR(ResizeObservation);

public:
    static WebIDL::ExceptionOr<GC::Ref<ResizeObservation>> create(JS::Realm&, DOM::Element&, Bindings::ResizeObserverBoxOptions);

    bool is_active();

    GC::Ref<DOM::Element> target() const { return m_target; }
    Bindings::ResizeObserverBoxOptions observed_box() const { return m_observed_box; }

    Vector<GC::Ref<ResizeObserverSize>>& last_reported_sizes() { return m_last_reported_sizes; }

    explicit ResizeObservation(JS::Realm& realm, DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box);

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<DOM::Element> m_target;
    Bindings::ResizeObserverBoxOptions m_observed_box;
    Vector<GC::Ref<ResizeObserverSize>> m_last_reported_sizes;
};

}
