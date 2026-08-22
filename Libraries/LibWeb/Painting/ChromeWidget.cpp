/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/Scrollbar.h>

namespace Web::Painting {

ChromeWidgetRegistry::ChromeWidgetRegistry() = default;

ChromeWidgetRegistry::~ChromeWidgetRegistry()
{
    clear();
}

RefPtr<Scrollbar> ChromeWidgetRegistry::scrollbar(Layout::RustFFI::NodeSlotId slot, ScrollDirection direction) const
{
    auto entry = m_entries.find(slot.index);
    if (entry == m_entries.end())
        return nullptr;
    return direction == ScrollDirection::Horizontal ? entry->value.horizontal_scrollbar : entry->value.vertical_scrollbar;
}

NonnullRefPtr<Scrollbar> ChromeWidgetRegistry::get_or_create_scrollbar(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot, ScrollDirection direction)
{
    auto& entry = m_entries.ensure(slot.index);
    auto& scrollbar = direction == ScrollDirection::Horizontal ? entry.horizontal_scrollbar : entry.vertical_scrollbar;
    if (!scrollbar)
        scrollbar = Scrollbar::create(arena, slot, direction);
    return *scrollbar;
}

RefPtr<ResizeHandle> ChromeWidgetRegistry::resize_handle(Layout::RustFFI::NodeSlotId slot) const
{
    auto entry = m_entries.find(slot.index);
    if (entry == m_entries.end())
        return nullptr;
    return entry->value.resize_handle;
}

NonnullRefPtr<ResizeHandle> ChromeWidgetRegistry::get_or_create_resize_handle(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot)
{
    auto& entry = m_entries.ensure(slot.index);
    if (!entry.resize_handle)
        entry.resize_handle = ResizeHandle::create(arena, slot);
    return *entry.resize_handle;
}

void ChromeWidgetRegistry::drop_widgets_for_slot(Layout::RustFFI::NodeSlotId slot)
{
    auto entry = m_entries.take(slot.index);
    if (!entry.has_value())
        return;
    if (entry->horizontal_scrollbar)
        entry->horizontal_scrollbar->detach({});
    if (entry->vertical_scrollbar)
        entry->vertical_scrollbar->detach({});
    if (entry->resize_handle)
        entry->resize_handle->detach({});
}

void ChromeWidgetRegistry::clear()
{
    for (auto& entry : m_entries) {
        if (entry.value.horizontal_scrollbar)
            entry.value.horizontal_scrollbar->detach({});
        if (entry.value.vertical_scrollbar)
            entry.value.vertical_scrollbar->detach({});
        if (entry.value.resize_handle)
            entry.value.resize_handle->detach({});
    }
    m_entries.clear();
}

ChromeWidget::ChromeWidget(Layout::NodeArena& arena, Layout::RustFFI::NodeSlotId slot)
    : m_arena(arena)
    , m_slot(slot)
{
}

Layout::Node* ChromeWidget::layout_node() const
{
    return layout_node_for_committed_slot(*m_arena, m_slot);
}

void ChromeWidget::detach(Badge<ChromeWidgetRegistry>)
{
    m_slot = Layout::RustFFI::NodeSlotId_INVALID;
    did_detach();
}

static bool is_canvas_background_source(Layout::NodeWithStyle const& layout_node)
{
    return layout_node.is_root_element() || body_background_is_propagated_to_root(layout_node);
}

static Color effective_scrollbar_background_color(Layout::NodeWithStyle const& layout_node)
{
    auto background_color = layout_node.document().canvas_background_color();

    Vector<Layout::NodeWithStyle const*, 32> ancestors;
    for (Layout::NodeWithStyle const* ancestor = &layout_node; ancestor; ancestor = ancestor->parent())
        ancestors.append(ancestor);

    for (auto const* ancestor : ancestors.in_reverse()) {
        auto const& layout_node_with_style = *ancestor;
        if (is_canvas_background_source(layout_node_with_style))
            continue;

        auto color = layout_node_with_style.background_color();
        if (color.alpha() == 0)
            continue;

        background_color = background_color.blend(color);
    }

    return background_color;
}

static CSS::ScrollbarColorData automatic_scrollbar_colors(Layout::NodeWithStyle const& layout_node)
{
    auto background_color = effective_scrollbar_background_color(layout_node);
    auto black_thumb = Color(Color::Black).with_alpha(128);
    auto white_thumb = Color(Color::White).with_alpha(128);

    auto black_thumb_contrast = background_color.contrast_ratio(background_color.blend(black_thumb));
    auto white_thumb_contrast = background_color.contrast_ratio(background_color.blend(white_thumb));
    auto thumb_color = black_thumb_contrast >= white_thumb_contrast ? black_thumb : white_thumb;

    return {
        .thumb_color = thumb_color,
        .track_color = thumb_color.with_alpha(25),
        .is_auto = true,
    };
}

CSS::ScrollbarColorData scrollbar_colors_for_paint(Layout::NodeWithStyle const& layout_node)
{
    auto scrollbar_colors = layout_node.scrollbar_color();
    if (!scrollbar_colors.is_auto)
        return scrollbar_colors;

    return automatic_scrollbar_colors(layout_node);
}

PhysicalResizeAxes physical_resize_axes(Layout::Node const& node)
{
    auto const& style_source = as<Layout::NodeWithStyle>(node);

    // https://drafts.csswg.org/css-ui/#resize
    if (style_source.resize() == CSS::Resize::None)
        return {};

    // 4.1. ... The resize property applies to elements that are scroll containers. UAs may also apply it,
    // regardless of the value of the overflow property, to:
    // - Replaced elements representing images or videos, such as img, video, picture, svg, object, or canvas.
    // - The <iframe> element.
    if (style_source.display().is_inline_outside() && style_source.display().is_flow_inside())
        return {};

    bool horizontal_writing_mode = style_source.writing_mode() == CSS::WritingMode::HorizontalTb;

    return {
        .horizontal = style_source.overflow_x() != CSS::Overflow::Visible
            && style_source.overflow_x() != CSS::Overflow::Clip
            && (style_source.resize() == CSS::Resize::Both
                || style_source.resize() == CSS::Resize::Horizontal
                || (style_source.resize() == CSS::Resize::Inline && horizontal_writing_mode)
                || (style_source.resize() == CSS::Resize::Block && !horizontal_writing_mode)),
        .vertical = style_source.overflow_y() != CSS::Overflow::Visible
            && style_source.overflow_y() != CSS::Overflow::Clip
            && (style_source.resize() == CSS::Resize::Both
                || style_source.resize() == CSS::Resize::Vertical
                || (style_source.resize() == CSS::Resize::Inline && !horizontal_writing_mode)
                || (style_source.resize() == CSS::Resize::Block && horizontal_writing_mode))
    };
}

bool has_resizer(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return false;

    // https://drafts.csswg.org/css-ui#resize
    if (node.is_viewport())
        return false;

    // The effect of the resize property on generated content is undefined.
    // Implementations should not apply the resize property to generated content.

    if (node.generated_for_pseudo_element().has_value())
        return false;

    auto axes = physical_resize_axes(node);
    return axes.horizontal || axes.vertical;
}

bool is_chrome_mirrored(Layout::Node const& node)
{
    auto const& style_source = as<Layout::NodeWithStyle>(node);
    auto writing_mode = style_source.writing_mode();
    return (writing_mode == CSS::WritingMode::HorizontalTb && style_source.direction() == CSS::Direction::Rtl)
        || writing_mode == CSS::WritingMode::VerticalRl
        || writing_mode == CSS::WritingMode::SidewaysRl;
}

Optional<CSSPixelRect> absolute_resizer_rect(Layout::Node const& node, ChromeMetrics const& metrics)
{
    if (!has_resizer(node))
        return {};
    auto padding_rect = absolute_padding_box_rect(node);
    CSSPixels x = is_chrome_mirrored(node) ? padding_rect.x() : padding_rect.right() - metrics.resize_gripper_size;
    CSSPixels y = padding_rect.bottom() - metrics.resize_gripper_size;
    return CSSPixelRect { x, y, metrics.resize_gripper_size, metrics.resize_gripper_size };
}

bool resizer_contains(Layout::Node const& node, CSSPixelPoint adjusted_position, ChromeMetrics const& metrics)
{
    if (!has_committed_box(node))
        return false;
    auto handle_rect = absolute_resizer_rect(node, metrics);
    if (!handle_rect.has_value())
        return false;
    bool bottom_left_resizer = is_chrome_mirrored(node);
    auto model = box_model(node);
    handle_rect->inflate(0, bottom_left_resizer ? 0 : model.border.right, model.border.bottom, bottom_left_resizer ? model.border.left : 0);

    return handle_rect->contains(adjusted_position);
}

static CSSPixels available_scrollbar_length(Layout::Node const& node, ScrollDirection direction, ChromeMetrics const& metrics)
{
    if (!has_committed_box(node))
        return {};
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto padding_rect = absolute_padding_box_rect(node);
    CSSPixels full_scrollport_length = is_horizontal ? padding_rect.width() : padding_rect.height();
    if (has_resizer(node))
        full_scrollport_length -= metrics.resize_gripper_size;
    else {
        if (is_horizontal && could_be_scrolled_by_wheel_event(node, ScrollDirection::Vertical))
            full_scrollport_length -= metrics.scroll_gutter_thickness;
        if (!is_horizontal && could_be_scrolled_by_wheel_event(node, ScrollDirection::Horizontal))
            full_scrollport_length -= metrics.scroll_gutter_thickness;
    }
    return full_scrollport_length;
}

Optional<CSSPixelRect> absolute_scrollbar_rect(Layout::Node const& node, ScrollDirection direction, bool with_gutter, ChromeMetrics const& metrics)
{
    if (!has_committed_box(node))
        return {};
    if (!could_be_scrolled_by_wheel_event(node, direction))
        return {};

    if (as<Layout::NodeWithStyle>(node).scrollbar_width() == CSS::ScrollbarWidth::None)
        return {};

    bool is_horizontal = direction == ScrollDirection::Horizontal;
    bool adjusting_for_resizer = has_resizer(node);

    CSSPixels rect_thickness = with_gutter
        ? metrics.scroll_gutter_thickness
        : metrics.scroll_thumb_thickness_thin + metrics.scroll_thumb_padding_thin;
    CSSPixelRect scrollbar_rect = absolute_padding_box_rect(node);

    if (is_horizontal) {
        if (!adjusting_for_resizer && could_be_scrolled_by_wheel_event(node, ScrollDirection::Vertical)) {
            scrollbar_rect.set_width(max(CSSPixels { 0 }, scrollbar_rect.width() - metrics.scroll_gutter_thickness));
            if (is_chrome_mirrored(node))
                scrollbar_rect.set_x(scrollbar_rect.x() + metrics.scroll_gutter_thickness);
        } else if (adjusting_for_resizer) {
            scrollbar_rect.set_width(available_scrollbar_length(node, ScrollDirection::Horizontal, metrics));
            if (is_chrome_mirrored(node))
                scrollbar_rect.set_x(scrollbar_rect.x() + metrics.resize_gripper_size);
        }
        scrollbar_rect.set_y(max(CSSPixels { 0 }, scrollbar_rect.bottom() - rect_thickness));
        scrollbar_rect.set_height(rect_thickness);
    } else {
        if (adjusting_for_resizer)
            scrollbar_rect.set_height(available_scrollbar_length(node, ScrollDirection::Vertical, metrics));
        if (!is_chrome_mirrored(node))
            scrollbar_rect.set_x(max(CSSPixels { 0 }, scrollbar_rect.right() - rect_thickness));
        scrollbar_rect.set_width(rect_thickness);
    }
    return scrollbar_rect;
}

Optional<ScrollbarData> compute_scrollbar_data(Layout::Node const& node, ScrollDirection direction, ChromeMetrics const& metrics, ScrollStateSnapshot const* scroll_state_snapshot, ScrollbarSizing scrollbar_sizing)
{
    if (!has_committed_box(node))
        return {};
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto const& style_source = as<Layout::NodeWithStyle>(node);
    auto overflow = is_horizontal ? style_source.overflow_x() : style_source.overflow_y();

    if (overflow != CSS::Overflow::Scroll && !could_be_scrolled_by_wheel_event(node, direction))
        return {};

    if (!own_scroll_node_index(node).value())
        return {};

    auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(node);
    if (!scrollable_overflow_rect.has_value())
        return {};

    CSSPixels scrollable_overflow_length = scrollable_overflow_rect->primary_size_for_orientation(orientation);
    if (scrollable_overflow_length == 0)
        return {};

    bool with_gutter = [&] {
        switch (scrollbar_sizing) {
        case ScrollbarSizing::Regular:
            return false;
        case ScrollbarSizing::Enlarged:
            return true;
        }
        VERIFY_NOT_REACHED();
    }();
    auto scrollbar_rect = absolute_scrollbar_rect(node, direction, with_gutter, metrics);
    if (!scrollbar_rect.has_value())
        return {};

    CSSPixels thumb_thickness = metrics.scroll_thumb_thickness_thin;
    CSSPixels thumb_margin = metrics.scroll_thumb_padding_thin;
    if (with_gutter) {
        thumb_thickness = metrics.scroll_thumb_thickness;
        thumb_margin = CSSPixels { (metrics.scroll_gutter_thickness - metrics.scroll_thumb_thickness) / 2.0 };
    }
    CSSPixels scrollbar_length = scrollbar_rect->primary_size_for_orientation(orientation);
    CSSPixels usable_scrollbar_length = max(CSSPixels { 0 }, scrollbar_length - (2 * thumb_margin));
    CSSPixels scrollport_size = absolute_padding_box_rect(node).primary_size_for_orientation(orientation);
    CSSPixels min_thumb_length = min(usable_scrollbar_length, metrics.scroll_thumb_min_length);
    CSSPixels thumb_length = max(usable_scrollbar_length * (scrollport_size / scrollable_overflow_length), min_thumb_length);

    ScrollbarData scrollbar_data = { .gutter_rect = {}, .thumb_rect = scrollbar_rect.value(), .track_rect = scrollbar_rect.value(), .thumb_travel_to_scroll_ratio = 0 };

    if (scrollable_overflow_length > scrollport_size)
        scrollbar_data.thumb_travel_to_scroll_ratio = (usable_scrollbar_length - thumb_length) / (scrollable_overflow_length - scrollport_size);

    scrollbar_data.thumb_rect.set_primary_size_for_orientation(orientation, thumb_length);
    scrollbar_data.thumb_rect.set_secondary_size_for_orientation(orientation, thumb_thickness);
    auto minimum_offset = minimum_scroll_offset(node).primary_offset_for_orientation(orientation);
    scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_margin - minimum_offset * scrollbar_data.thumb_travel_to_scroll_ratio);
    if (with_gutter || (!is_horizontal && is_chrome_mirrored(node)))
        scrollbar_data.thumb_rect.translate_secondary_offset_for_orientation(orientation, thumb_margin);
    if (with_gutter)
        scrollbar_data.gutter_rect = scrollbar_rect.value();

    if (scroll_state_snapshot) {
        auto own_offset = scroll_state_snapshot->device_offset_for_index(own_scroll_node_index(node));
        auto device_scroll_offset = is_horizontal ? -own_offset.x() : -own_offset.y();
        auto device_pixels_per_css_pixel = static_cast<float>(node.document().page().client().device_pixels_per_css_pixel());
        CSSPixels thumb_offset = CSSPixels::nearest_value_for(device_scroll_offset / device_pixels_per_css_pixel) * scrollbar_data.thumb_travel_to_scroll_ratio;
        scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_offset);
    }

    return scrollbar_data;
}

}
