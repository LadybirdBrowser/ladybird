/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibWeb/Bindings/ResizeObserverSizePrototype.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/ResizeObserver/ResizeObserverSize.h>

namespace Web::ResizeObserver {

JS_DEFINE_ALLOCATOR(ResizeObserverSize);

void ResizeObserverSize::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ResizeObserverSize);
}

// https://drafts.csswg.org/resize-observer-1/#calculate-box-size
JS::NonnullGCPtr<ResizeObserverSize> ResizeObserverSize::calculate_box_size(JS::Realm& realm, DOM::Element& target, Bindings::ResizeObserverBoxOptions observed_box)
{
    // 1. Let computedSize be a new ResizeObserverSize object.
    auto computed_size = realm.heap().allocate<ResizeObserverSize>(realm, realm);

    // FIXME: 2. If target is an SVGGraphicsElement that does not have an associated CSS layout box:
    // Otherwise:
    if (target.paintable_box()) {
        auto const& paintable_box = *target.paintable_box();
        switch (observed_box) {
        case Bindings::ResizeObserverBoxOptions::BorderBox:
            // 1. Set computedSize’s inlineSize attribute to target’s border area inline length.
            computed_size->set_inline_size(paintable_box.border_box_width().to_double());
            // 2. Set computedSize’s blockSize attribute to target’s border area block length.
            computed_size->set_block_size(paintable_box.border_box_height().to_double());
            break;
        case Bindings::ResizeObserverBoxOptions::ContentBox:
            // 1. Set computedSize’s inlineSize attribute to target’s content area inline length.
            computed_size->set_inline_size(paintable_box.content_width().to_double());
            // 2. Set computedSize’s blockSize attribute to target’s content area block length.
            computed_size->set_block_size(paintable_box.content_height().to_double());
            break;
        case Bindings::ResizeObserverBoxOptions::DevicePixelContentBox: {
            auto device_pixel_ratio = target.document().window()->device_pixel_ratio();
            // 1. Set computedSize’s inlineSize attribute to target’s content area inline length, in integral device pixels.
            computed_size->set_inline_size(paintable_box.border_box_width().to_double() * device_pixel_ratio);
            // 2. Set computedSize’s blockSize attribute to target’s content area block length, in integral device pixels.
            computed_size->set_block_size(paintable_box.border_box_height().to_double() * device_pixel_ratio);
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }

    // 3. Return computedSize.s
    return computed_size;
}

bool ResizeObserverSize::equals(ResizeObserverSize const& other) const
{
    return m_inline_size == other.m_inline_size && m_block_size == other.m_block_size;
}

}
