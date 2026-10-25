/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/Zoom.h>
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
    Layout::ForcedReadScope read { target.document() };
    RawSize size;

    // FIXME: If target is an SVGGraphicsElement that does not have an associated CSS layout box:
    // Otherwise:
    // NB: Layout was up to date when observations were gathered, but a previous
    //     observer's callback may have invalidated it before we get here.
    //     This matches the behavior of all major browsers.
    auto const* layout_node = target.unsafe_layout_node(read);
    // INTEROP: Non-replaced inline boxes have no content area to observe, so they are reported with a size of 0x0.
    if (layout_node && !layout_node->is_inline_node() && Painting::has_committed_box(*layout_node)) {
        double width = 0;
        double height = 0;
        // https://drafts.csswg.org/css-viewport/#zoom-om
        // All other APIs related to element geometries must return unscaled lengths (except as detailed below).
        // NB: The device pixel content box is measured in device pixels, so it stays scaled.
        auto const zoom = CSS::effective_zoom(*layout_node);
        switch (observed_box) {
        // -> If observedBox is "border-box"
        case ObservedBox::BorderBox:
            // 1. Set computedSize's inlineSize attribute to target's border area inline length.
            // 2. Set computedSize's blockSize attribute to target's border area block length.
            width = CSS::unscaled(Painting::border_box_width(*layout_node).to_double(), zoom);
            height = CSS::unscaled(Painting::border_box_height(*layout_node).to_double(), zoom);
            break;
        // -> If observedBox is "content-box"
        case ObservedBox::ContentBox:
            // 1. Set computedSize's inlineSize attribute to target's content area inline length.
            // 2. Set computedSize's blockSize attribute to target's content area block length.
            width = CSS::unscaled(Painting::content_width(*layout_node).to_double(), zoom);
            height = CSS::unscaled(Painting::content_height(*layout_node).to_double(), zoom);
            break;
        // -> If observedBox is "device-pixel-content-box"
        case ObservedBox::DevicePixelContentBox: {
            // 1. Set computedSize's inlineSize attribute to target's content area inline length, in integral device
            //    pixels.
            // 2. Set computedSize's blockSize attribute to target's content area block length, in integral device
            //    pixels.
            auto device_pixel_ratio = target.document().window()->device_pixel_ratio();
            width = AK::round(Painting::content_width(*layout_node).to_double() * device_pixel_ratio);
            height = AK::round(Painting::content_height(*layout_node).to_double() * device_pixel_ratio);
            break;
        }
        }

        // NB: The inline and block lengths follow the writing mode of the target.
        if (layout_node->writing_mode() == CSS::WritingMode::HorizontalTb) {
            size.inline_size = width;
            size.block_size = height;
        } else {
            size.inline_size = height;
            size.block_size = width;
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
