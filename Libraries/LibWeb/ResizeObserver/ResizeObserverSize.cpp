/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/ResizeObserverSizePrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/ResizeObserver/ResizeObserverSize.h>

namespace Web::ResizeObserver {

GC_DEFINE_ALLOCATOR(ResizeObserverSize);

void ResizeObserverSize::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ResizeObserverSize);
    Base::initialize(realm);
}

// https://drafts.csswg.org/resize-observer-1/#calculate-box-size
ResizeObserverSize::RawSize ResizeObserverSize::compute_box_size(DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box)
{
    RawSize size;

    // FIXME: If target is an SVGGraphicsElement that does not have an associated CSS layout box:
    // Otherwise:
    // NB: Layout was up to date when observations were gathered, but a previous
    //     observer's callback may have invalidated it before we get here.
    //     This matches the behavior of all major browsers.
    if (target.unsafe_paintable_box()) {
        auto const& paintable_box = *target.unsafe_paintable_box();
        switch (observed_box) {
        case Bindings::ResizeObserverBoxOptions::BorderBox:
            size.inline_size = paintable_box.border_box_width().to_double();
            size.block_size = paintable_box.border_box_height().to_double();
            break;
        case Bindings::ResizeObserverBoxOptions::ContentBox:
            size.inline_size = paintable_box.content_width().to_double();
            size.block_size = paintable_box.content_height().to_double();
            break;
        case Bindings::ResizeObserverBoxOptions::DevicePixelContentBox: {
            auto device_pixel_ratio = target.document().window()->device_pixel_ratio();
            size.inline_size = paintable_box.border_box_width().to_double() * device_pixel_ratio;
            size.block_size = paintable_box.border_box_height().to_double() * device_pixel_ratio;
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }

    return size;
}

GC::Ref<ResizeObserverSize> ResizeObserverSize::calculate_box_size(JS::Realm& realm, DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box)
{
    auto raw = compute_box_size(target, observed_box);
    auto computed_size = realm.create<ResizeObserverSize>(realm);
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
