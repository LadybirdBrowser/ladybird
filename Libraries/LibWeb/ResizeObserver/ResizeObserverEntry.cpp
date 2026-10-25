/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/Zoom.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/ResizeObserver/ResizeObserverEntry.h>

namespace Web::ResizeObserver {

GC_DEFINE_ALLOCATOR(ResizeObserverEntry);

// https://drafts.csswg.org/resize-observer-1/#create-and-populate-resizeobserverentry-h
WebIDL::ExceptionOr<GC::Ref<ResizeObserverEntry>> ResizeObserverEntry::create_and_populate(Layout::BegunRead const& read, DOM::Element& target)
{
    // 1. Let this be a new ResizeObserverEntry.
    // 2. Set this.target slot to target.
    auto resize_observer_entry = GC::Heap::the().allocate<ResizeObserverEntry>(target);

    // 3. Set this.borderBoxSize slot to result of calculating box size given target and observedBox of "border-box".
    auto border_box_size = ResizeObserverSize::calculate_box_size(target, ObservedBox::BorderBox);
    resize_observer_entry->m_border_box_size.append(border_box_size);

    // 4. Set this.contentBoxSize slot to result of calculating box size given target and observedBox of "content-box".
    auto content_box_size = ResizeObserverSize::calculate_box_size(target, ObservedBox::ContentBox);
    resize_observer_entry->m_content_box_size.append(content_box_size);

    // 5. Set this.devicePixelContentBoxSize slot to result of calculating box size given target and observedBox of "device-pixel-content-box".
    auto device_pixel_content_box_size = ResizeObserverSize::calculate_box_size(target, ObservedBox::DevicePixelContentBox);
    resize_observer_entry->m_device_pixel_content_box_size.append(device_pixel_content_box_size);

    // 6. Set this.contentRect to logical this.contentBoxSize given target and observedBox of "content-box".
    // NB: The contentRect is in physical coordinates, so the logical sizes are mapped back to a width and a height.
    double x = 0;
    double y = 0;
    double width = content_box_size->inline_size();
    double height = content_box_size->block_size();

    // NB: Layout was up to date when observations were gathered, but a previous
    //     observer's callback may have invalidated it before we get here.
    //     This matches the behavior of all major browsers.
    auto const* layout_node = target.unsafe_layout_node(read);
    if (layout_node && layout_node->writing_mode() != CSS::WritingMode::HorizontalTb)
        swap(width, height);

    // 7. If target is not an SVG element or target is an SVG element with an associated CSS layout box do these steps:
    if (!target.is_svg_element() && layout_node && !layout_node->is_inline_node() && Painting::has_committed_box(*layout_node)) {
        auto box_model = Painting::box_model(*layout_node);
        // NB: The padding is unscaled, as the sizes are.
        //     https://drafts.csswg.org/css-viewport/#zoom-om
        auto const zoom = CSS::effective_zoom(*layout_node);
        // Set this.contentRect.top to target.padding top.
        y = CSS::unscaled(box_model.padding.top.to_double(), zoom);
        // Set this.contentRect.left to target.padding left.
        x = CSS::unscaled(box_model.padding.left.to_double(), zoom);
    } else if (target.is_svg_element() && layout_node && Painting::has_committed_box(*layout_node)) {
        // 8. If target is an SVG element without an associated CSS layout box do these steps:
        // Set this.contentRect.top and this.contentRect.left to 0.
        // NOTE: This is already done by the default constructor.
    }
    resize_observer_entry->m_content_rect = Geometry::DOMRectReadOnly::create(x, y, width, height);

    return resize_observer_entry;
}

void ResizeObserverEntry::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target);
    visitor.visit(m_content_box_size);
    visitor.visit(m_border_box_size);
    visitor.visit(m_device_pixel_content_box_size);
    visitor.visit(m_content_rect);
}

}
