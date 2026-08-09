/*
 * Copyright (c) 2025, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Page/ElementResizeAction.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/Paintable.h>

// https://drafts.csswg.org/css-ui#resize

namespace Web {

static Optional<CSSPixelSize> containing_block_padding_box_size(Layout::Node const& layout_node)
{
    auto parent_box = layout_node.containing_block();
    if (!parent_box)
        return {};
    if (auto paintable = parent_box->paintable(); auto const* paintable_box = paintable.ptr())
        return paintable_box->absolute_padding_box_rect().size();
    return {};
}

ElementResizeAction::ElementResizeAction(GC::Ref<DOM::Element> element, CSSPixelPoint pointer_down_origin)
    : m_element(element)
    , m_pointer_down_origin(pointer_down_origin)
{
    auto paintable_box = element->paintable_box();
    if (paintable_box)
        m_initial_border_box_size = paintable_box->absolute_border_box_rect().size();
}

void ElementResizeAction::handle_pointer_move(CSSPixelPoint pointer_position)
{
    auto element = m_element.ptr();
    if (!element || !element->is_connected())
        return;

    auto paintable_box = element->paintable_box();
    if (!paintable_box)
        return;
    auto const& layout_node = paintable_box->layout_node();
    auto resize = layout_node.resize();
    if (resize == CSS::Resize::None)
        return;

    bool horizontal_writing_mode = layout_node.writing_mode() == CSS::WritingMode::HorizontalTb;
    bool resize_x = resize == CSS::Resize::Both
        || resize == CSS::Resize::Horizontal
        || (resize == CSS::Resize::Inline && horizontal_writing_mode)
        || (resize == CSS::Resize::Block && !horizontal_writing_mode);

    bool resize_y = resize == CSS::Resize::Both
        || resize == CSS::Resize::Vertical
        || (resize == CSS::Resize::Inline && !horizontal_writing_mode)
        || (resize == CSS::Resize::Block && horizontal_writing_mode);

    CSSPixels dx = resize_x ? pointer_position.x() - m_pointer_down_origin.x() : 0;
    CSSPixels dy = resize_y ? pointer_position.y() - m_pointer_down_origin.y() : 0;
    auto writing_mode = layout_node.writing_mode();
    if ((writing_mode == CSS::WritingMode::HorizontalTb && layout_node.direction() == CSS::Direction::Rtl)
        || writing_mode == CSS::WritingMode::VerticalRl
        || writing_mode == CSS::WritingMode::SidewaysRl) {
        dx = -dx;
    }
    CSSPixels css_width = max(ChromeMetrics::ZOOM_INVARIANT_RESIZE_GRIPPER_SIZE, m_initial_border_box_size.width() + dx);
    CSSPixels css_height = max(ChromeMetrics::ZOOM_INVARIANT_RESIZE_GRIPPER_SIZE, m_initial_border_box_size.height() + dy);

    auto reference_basis = containing_block_padding_box_size(layout_node);

    if (reference_basis.has_value()) {
        if (auto const& min_width = layout_node.min_width(); !min_width.is_auto()) {
            css_width = max(css_width, min_width.to_px(reference_basis->width()));
        }
        if (auto const& max_width = layout_node.max_width(); !max_width.is_none()) {
            css_width = min(css_width, max_width.to_px(reference_basis->width()));
        }
        if (auto const& min_height = layout_node.min_height(); !min_height.is_auto()) {
            css_height = max(css_height, min_height.to_px(reference_basis->height()));
        }
        if (auto const& max_height = layout_node.max_height(); !max_height.is_none()) {
            css_height = min(css_height, max_height.to_px(reference_basis->height()));
        }
    }
    if (layout_node.box_sizing() == CSS::BoxSizing::ContentBox) {
        auto const& metrics = paintable_box->box_model();
        css_width -= metrics.padding.left + metrics.padding.right + layout_node.border_left().width + layout_node.border_right().width;
        css_height -= metrics.padding.top + metrics.padding.bottom + layout_node.border_top().width + layout_node.border_bottom().width;
    }

    auto style = element->style();
    auto width_str = Utf16String::formatted("{:.2f}px", max(0.0, css_width.to_double()));
    auto height_str = Utf16String::formatted("{:.2f}px", max(0.0, css_height.to_double()));

    MUST(style->set_property(CSS::PropertyID::Width, width_str.utf16_view()));
    MUST(style->set_property(CSS::PropertyID::Height, height_str.utf16_view()));
}

}
