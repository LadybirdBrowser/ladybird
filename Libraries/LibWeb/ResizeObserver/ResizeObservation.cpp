/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/ResizeObserver/ResizeObservation.h>

namespace Web::ResizeObserver {

GC_DEFINE_ALLOCATOR(ResizeObservation);

WebIDL::ExceptionOr<GC::Ref<ResizeObservation>> ResizeObservation::create(DOM::Element& target, ObservedBox observed_box)
{
    return GC::Heap::the().allocate<ResizeObservation>(target, observed_box);
}

ResizeObservation::ResizeObservation(DOM::Element& target, ObservedBox observed_box)
    : m_target(target)
    , m_observed_box(observed_box)
{
    m_last_reported_sizes.append({});
}

void ResizeObservation::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

// https://drafts.csswg.org/resize-observer-1/#dom-resizeobservation-isactive
bool ResizeObservation::is_active()
{
    if (!m_target)
        return false;

    // 1. Set currentSize by calculate box size given target and observedBox.
    auto current_size = ResizeObserverSize::compute_box_size(*m_target, m_observed_box);

    // 2. Return true if currentSize is not equal to the first entry in this.lastReportedSizes.
    VERIFY(!m_last_reported_sizes.is_empty());
    auto const& last_reported_size = m_last_reported_sizes.first();
    if (last_reported_size.inline_size != current_size.inline_size || last_reported_size.block_size != current_size.block_size)
        return true;

    // 3. Return false.
    return false;
}

void ResizeObservation::set_last_reported_sizes(ReadonlySpan<GC::Ref<ResizeObserverSize>> sizes)
{
    m_last_reported_sizes.clear();
    m_last_reported_sizes.ensure_capacity(sizes.size());
    for (auto const& size : sizes) {
        m_last_reported_sizes.unchecked_append({
            .inline_size = size->inline_size(),
            .block_size = size->block_size(),
        });
    }
}

}
