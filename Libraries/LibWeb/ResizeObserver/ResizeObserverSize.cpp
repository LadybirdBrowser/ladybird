/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/ResizeObserver/ResizeObserverSize.h>

namespace Web::ResizeObserver {

GC_DEFINE_ALLOCATOR(ResizeObserverSize);

// https://drafts.csswg.org/resize-observer-1/#calculate-box-size
ResizeObserverSize::RawSize ResizeObserverSize::compute_box_size(DOM::Element& target, ObservedBox observed_box)
{
    RawSize size;

    // FIXME: If target is an SVGGraphicsElement that does not have an associated CSS layout box:
    // Otherwise:
    // NB: Layout was up to date when observations were gathered, but a previous
    //     observer's callback may have invalidated it before we get here.
    //     This matches the behavior of all major browsers.
    auto const* layout_node = target.unsafe_layout_node();
    if (layout_node && Painting::has_committed_box(*layout_node)) {
        switch (observed_box) {
        case ObservedBox::BorderBox:
            size.inline_size = Painting::border_box_width(*layout_node).to_double();
            size.block_size = Painting::border_box_height(*layout_node).to_double();
            break;
        case ObservedBox::ContentBox:
            size.inline_size = Painting::content_width(*layout_node).to_double();
            size.block_size = Painting::content_height(*layout_node).to_double();
            break;
        case ObservedBox::DevicePixelContentBox: {
            auto device_pixel_ratio = target.document().window()->device_pixel_ratio();
            size.inline_size = Painting::border_box_width(*layout_node).to_double() * device_pixel_ratio;
            size.block_size = Painting::border_box_height(*layout_node).to_double() * device_pixel_ratio;
            break;
        }
        }
    }

    return size;
}

GC::Ref<ResizeObserverSize> ResizeObserverSize::calculate_box_size(DOM::Element& target, ObservedBox observed_box)
{
    auto raw = compute_box_size(target, observed_box);
    auto computed_size = GC::Heap::the().allocate<ResizeObserverSize>();
    computed_size->set_inline_size(raw.inline_size);
    computed_size->set_block_size(raw.block_size);
    return computed_size;
}

bool ResizeObserverSize::equals(RawSize const& other) const
{
    return m_inline_size == other.inline_size && m_block_size == other.block_size;
}

bool ResizeObserverSize::equals(ResizeObserverSize const& other) const
{
    return m_inline_size == other.m_inline_size && m_block_size == other.m_block_size;
}

}
