/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/ResizeObserver.h>
#include <LibWeb/ResizeObserver/ResizeObserverSize.h>

namespace Web::ResizeObserver {

// https://drafts.csswg.org/resize-observer-1/#resize-observation-interface
class ResizeObservation : public JS::Cell {
    GC_CELL(ResizeObservation, JS::Cell);
    GC_DECLARE_ALLOCATOR(ResizeObservation);

public:
    static WebIDL::ExceptionOr<GC::Ref<ResizeObservation>> create(DOM::Element&, Bindings::ResizeObserverBoxOptions);

    bool is_active();

    GC::Ptr<DOM::Element> target() const { return m_target.ptr(); }
    Bindings::ResizeObserverBoxOptions observed_box() const { return m_observed_box; }

    void set_last_reported_sizes(ReadonlySpan<GC::Ref<ResizeObserverSize>>);

    explicit ResizeObservation(DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box);

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Weak<DOM::Element> m_target;
    Bindings::ResizeObserverBoxOptions m_observed_box;
    Vector<ResizeObserverSize::RawSize> m_last_reported_sizes;
};

}
