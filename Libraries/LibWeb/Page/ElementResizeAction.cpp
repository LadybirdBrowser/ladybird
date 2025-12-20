/*
 * Copyright (c) 2025, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Page/ChromeMetrics.h>
#include <LibWeb/Page/ElementResizeAction.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web {

static Optional<CSSPixelSize> containing_block_padding_box_size(Layout::Node const& layout_node)
{
    auto parent_box = layout_node.containing_block();
    if (!parent_box)
        return {};

    auto const* parent_paintable = parent_box->first_paintable();
    if (!parent_paintable || !parent_paintable->is_paintable_box())
        return {};

    return static_cast<Painting::PaintableBox const*>(parent_paintable)->absolute_padding_box_rect().size();
}

ElementResizeAction::ElementResizeAction(GC::Ref<DOM::Element> element, CSSPixelPoint pointer_down_origin)
    : m_element(element)
    , m_pointer_down_origin(pointer_down_origin)
{
    auto const* paintable_box = m_element->paintable_box();
    if (paintable_box)
        m_initial_border_box_size = paintable_box->absolute_border_box_rect().size();
}

void ElementResizeAction::handle_pointer_move(CSSPixelPoint pointer_position)
{
    auto const* paintable_box = m_element->paintable_box();
    if (!paintable_box)
        return;
    auto const& layout_node = paintable_box->layout_node();
    auto const& computed = layout_node.computed_values();
    auto resize = computed.resize();
    if (resize == CSS::Resize::None)
        return;

    CSSPixels dx = resize == CSS::Resize::Vertical ? 0 : pointer_position.x() - m_pointer_down_origin.x();
    CSSPixels dy = resize == CSS::Resize::Horizontal ? 0 : pointer_position.y() - m_pointer_down_origin.y();
    auto writing_mode = computed.writing_mode();
    if ((writing_mode == CSS::WritingMode::HorizontalTb && computed.direction() == CSS::Direction::Rtl)
        || writing_mode == CSS::WritingMode::VerticalRl
        || writing_mode == CSS::WritingMode::SidewaysRl) {
        dx = -dx;
    }
    CSSPixels css_width = max(ChromeMetrics::ResizeGripperSize, m_initial_border_box_size.width() + dx);
    CSSPixels css_height = max(ChromeMetrics::ResizeGripperSize, m_initial_border_box_size.height() + dy);

    auto reference_basis = containing_block_padding_box_size(layout_node);

    if (reference_basis.has_value()) {
        if (auto const& min_width = computed.min_width(); !min_width.is_auto()) {
            css_width = max(css_width, min_width.to_px(layout_node, reference_basis->width()));
        }
        if (auto const& max_width = computed.max_width(); !max_width.is_none()) {
            css_width = min(css_width, max_width.to_px(layout_node, reference_basis->width()));
        }
        if (auto const& min_height = computed.min_height(); !min_height.is_auto()) {
            css_height = max(css_height, min_height.to_px(layout_node, reference_basis->height()));
        }
        if (auto const& max_height = computed.max_height(); !max_height.is_none()) {
            css_height = min(css_height, max_height.to_px(layout_node, reference_basis->height()));
        }
    }
    if (computed.box_sizing() == CSS::BoxSizing::ContentBox) {
        auto const& metrics = paintable_box->box_model();
        css_width -= metrics.padding.left + metrics.padding.right + computed.border_left().width + computed.border_right().width;
        css_height -= metrics.padding.top + metrics.padding.bottom + computed.border_top().width + computed.border_bottom().width;
    }

    auto style = m_element->style_for_bindings();
    auto width_str = MUST(String::formatted("{:.2f}px", max(0.0, css_width.to_double())));
    auto height_str = MUST(String::formatted("{:.2f}px", max(0.0, css_height.to_double())));

    MUST(style->set_property(CSS::PropertyID::Width, width_str));
    MUST(style->set_property(CSS::PropertyID::Height, height_str));
    CSSPixelRect invalidated = paintable_box->absolute_border_box_rect();
    invalidated.set_size(css_width, css_height);
    m_element->document().set_needs_display(invalidated, InvalidateDisplayList::No);
}

void ElementResizeAction::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(m_element);
}

}
