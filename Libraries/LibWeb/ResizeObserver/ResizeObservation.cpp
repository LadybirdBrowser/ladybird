/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/ResizeObserver/ResizeObservation.h>

namespace Web::ResizeObserver {

GC_DEFINE_ALLOCATOR(ResizeObservation);

WebIDL::ExceptionOr<GC::Ref<ResizeObservation>> ResizeObservation::create(JS::Realm& realm, DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box)
{
    return realm.create<ResizeObservation>(realm, target, observed_box);
}

ResizeObservation::ResizeObservation(JS::Realm& realm, DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box)
    : m_realm(realm)
    , m_target(target)
    , m_observed_box(observed_box)
{
    auto computed_size = realm.create<ResizeObserverSize>(realm);
    m_last_reported_sizes.append(computed_size);
}

void ResizeObservation::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_target);
    visitor.visit(m_last_reported_sizes);
}

// https://drafts.csswg.org/resize-observer-1/#dom-resizeobservation-isactive
bool ResizeObservation::is_active()
{
    // 1. Set currentSize by calculate box size given target and observedBox.
    auto current_size = ResizeObserverSize::calculate_box_size(m_realm, m_target, m_observed_box);

    // 2. Return true if currentSize is not equal to the first entry in this.lastReportedSizes.
    VERIFY(!m_last_reported_sizes.is_empty());
    if (!m_last_reported_sizes.first()->equals(*current_size))
        return true;

    // 3. Return false.
    return false;
}

}
