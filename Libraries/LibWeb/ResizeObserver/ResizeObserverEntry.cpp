/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ResizeObserverEntryPrototype.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/ResizeObserver/ResizeObserverEntry.h>

namespace Web::ResizeObserver {

JS_DEFINE_ALLOCATOR(ResizeObserverEntry);

// https://drafts.csswg.org/resize-observer-1/#create-and-populate-resizeobserverentry-h
WebIDL::ExceptionOr<JS::NonnullGCPtr<ResizeObserverEntry>> ResizeObserverEntry::create_and_populate(JS::Realm& realm, DOM::Element& target)
{
    // 1. Let this be a new ResizeObserverEntry.
    // 2. Set this.target slot to target.
    auto resize_observer_entry = realm.create<ResizeObserverEntry>(realm, target);

    // 3. Set this.borderBoxSize slot to result of calculating box size given target and observedBox of "border-box".
    auto border_box_size = ResizeObserverSize::calculate_box_size(realm, target, Bindings::ResizeObserverBoxOptions::BorderBox);
    resize_observer_entry->m_border_box_size.append(border_box_size);

    // 4. Set this.contentBoxSize slot to result of calculating box size given target and observedBox of "content-box".
    auto content_box_size = ResizeObserverSize::calculate_box_size(realm, target, Bindings::ResizeObserverBoxOptions::ContentBox);
    resize_observer_entry->m_content_box_size.append(content_box_size);

    // 5. Set this.devicePixelContentBoxSize slot to result of calculating box size given target and observedBox of "device-pixel-content-box".
    auto device_pixel_content_box_size = ResizeObserverSize::calculate_box_size(realm, target, Bindings::ResizeObserverBoxOptions::DevicePixelContentBox);
    resize_observer_entry->m_device_pixel_content_box_size.append(device_pixel_content_box_size);

    // 6. Set this.contentRect to logical this.contentBoxSize given target and observedBox of "content-box".
    double x = 0;
    double y = 0;
    double width = content_box_size->inline_size();
    double height = content_box_size->block_size();

    // 7. If target is not an SVG element or target is an SVG element with an associated CSS layout box do these steps:
    if (!target.is_svg_element() && target.paintable_box()) {
        auto const& paintable_box = *target.paintable_box();
        auto absolute_padding_rect = paintable_box.absolute_padding_box_rect();
        // Set this.contentRect.top to target.padding top.
        y = absolute_padding_rect.y().to_double();
        // Set this.contentRect.left to target.padding left.
        x = absolute_padding_rect.x().to_double();
    } else if (target.is_svg_element() && target.paintable_box()) {
        // 8. If target is an SVG element without an associated CSS layout box do these steps:
        // Set this.contentRect.top and this.contentRect.left to 0.
        // NOTE: This is already done by the default constructor.
    }
    resize_observer_entry->m_content_rect = MUST(Geometry::DOMRectReadOnly::construct_impl(realm, x, y, width, height));

    return resize_observer_entry;
}

void ResizeObserverEntry::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ResizeObserverEntry);
}

void ResizeObserverEntry::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target);
    visitor.visit(m_content_box_size);
    visitor.visit(m_border_box_size);
    visitor.visit(m_device_pixel_content_box_size);
    visitor.visit(m_content_rect);
}

static JS::NonnullGCPtr<JS::Object> to_js_array(JS::Realm& realm, Vector<JS::NonnullGCPtr<ResizeObserverSize>> const& sizes)
{
    JS::MarkedVector<JS::Value> vector(realm.heap());
    for (auto const& size : sizes)
        vector.append(JS::Value(size.ptr()));

    auto array = JS::Array::create_from(realm, vector);
    MUST(array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    return array;
}

JS::NonnullGCPtr<JS::Object> ResizeObserverEntry::border_box_size_js_array() const
{
    return to_js_array(realm(), m_border_box_size);
}

JS::NonnullGCPtr<JS::Object> ResizeObserverEntry::content_box_size_js_array() const
{
    return to_js_array(realm(), m_content_box_size);
}

JS::NonnullGCPtr<JS::Object> ResizeObserverEntry::device_pixel_content_box_size_js_array() const
{
    return to_js_array(realm(), m_device_pixel_content_box_size);
}

}
