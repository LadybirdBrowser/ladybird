/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/SVGPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/ShadowPainting.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/TableBordersPainting.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGFilterElement.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(PaintableBox);

static bool g_paint_viewport_scrollbars = true;

namespace {

struct PhysicalResizeAxes {
    bool horizontal;
    bool vertical;
};

}

static PhysicalResizeAxes compute_physical_resize_axes(CSS::ComputedValues const& computed);

void set_paint_viewport_scrollbars(bool const enabled)
{
    g_paint_viewport_scrollbars = enabled;
}

GC::Ref<PaintableBox> PaintableBox::create(Layout::Box const& layout_box)
{
    return layout_box.heap().allocate<PaintableBox>(layout_box);
}

GC::Ref<PaintableBox> PaintableBox::create(Layout::InlineNode const& layout_box)
{
    return layout_box.heap().allocate<PaintableBox>(layout_box);
}

PaintableBox::PaintableBox(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

PaintableBox::PaintableBox(Layout::InlineNode const& layout_box)
    : Paintable(layout_box)
{
}

PaintableBox::~PaintableBox()
{
}

void PaintableBox::reset_for_relayout()
{
    if (parent())
        remove();
    while (first_child())
        first_child()->remove();

    m_containing_block = {};

    m_needs_paint_only_properties_update = true;

    m_offset = {};
    m_content_size = {};

    m_box_model = {};

    m_overflow_data.clear();
    m_override_borders_data.clear();
    m_table_cell_coordinates.clear();
    m_sticky_insets = nullptr;

    m_absolute_rect.clear();
    m_absolute_padding_box_rect.clear();
    m_absolute_border_box_rect.clear();

    m_enclosing_scroll_frame = nullptr;
    m_own_scroll_frame = nullptr;
    m_accumulated_visual_context = nullptr;
    m_accumulated_visual_context_for_descendants = nullptr;

    m_used_values_for_grid_template_columns = nullptr;
    m_used_values_for_grid_template_rows = nullptr;

    invalidate_stacking_context();
}

void PaintableBox::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stacking_context);
}

CSSPixelPoint PaintableBox::scroll_offset() const
{
    if (is_viewport_paintable()) {
        auto navigable = document().navigable();
        VERIFY(navigable);
        return navigable->viewport_scroll_offset();
    }

    auto const& node = layout_node();
    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value())
        return node.pseudo_element_generator()->scroll_offset(*pseudo_element);

    if (auto const* element = as_if<DOM::Element>(dom_node().ptr()))
        return element->scroll_offset({});
    return {};
}

PaintableBox::ScrollHandled PaintableBox::set_scroll_offset(CSSPixelPoint offset)
{
    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return ScrollHandled::No;

    auto padding_rect = absolute_padding_box_rect();
    auto max_x_offset = max(scrollable_overflow_rect->width() - padding_rect.width(), 0);
    auto max_y_offset = max(scrollable_overflow_rect->height() - padding_rect.height(), 0);

    offset.set_x(clamp(offset.x(), 0, max_x_offset));
    offset.set_y(clamp(offset.y(), 0, max_y_offset));

    // FIXME: If there is horizontal and vertical scroll ignore only part of the new offset
    if (offset.y() < 0 || scroll_offset() == offset)
        return ScrollHandled::No;

    if (is_viewport_paintable()) {
        auto navigable = document().navigable();
        VERIFY(navigable);
        navigable->perform_scroll_of_viewport_scrolling_box(offset);
        return ScrollHandled::Yes;
    }

    document().set_needs_to_refresh_scroll_state(true);

    auto& node = layout_node();
    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value()) {
        node.pseudo_element_generator()->set_scroll_offset(*pseudo_element, offset);
    } else if (auto* element = as_if<DOM::Element>(*dom_node())) {
        element->set_scroll_offset({}, offset);
    } else {
        return ScrollHandled::No;
    }

    // https://drafts.csswg.org/cssom-view-1/#scrolling-events
    // Whenever an element gets scrolled (whether in response to user interaction or by an API),
    // the user agent must run these steps:

    // 1. Let doc be the element’s node document.
    auto& document = layout_node().document();

    // FIXME: 2. If the element is a snap container, run the steps to update snapchanging targets for the element with
    //           the element’s eventual snap target in the block axis as newBlockTarget and the element’s eventual snap
    //           target in the inline axis as newInlineTarget.

    GC::Ptr<DOM::EventTarget> event_target;
    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value())
        event_target = node.pseudo_element_generator();
    else
        event_target = dom_node();

    if (!event_target)
        return ScrollHandled::Yes;

    // 3. If (element, "scroll") is already in doc’s pending scroll events, abort these steps.
    if (document.pending_scroll_events().contains_slow(DOM::Document::PendingScrollEvent { *event_target, HTML::EventNames::scroll }))
        return ScrollHandled::Yes;

    // 4. Append (element, "scroll") to doc’s pending scroll events.
    document.pending_scroll_events().append({ *event_target, HTML::EventNames::scroll });

    set_needs_display(InvalidateDisplayList::No);
    return ScrollHandled::Yes;
}

PaintableBox::ScrollHandled PaintableBox::scroll_by(int delta_x, int delta_y)
{
    return set_scroll_offset(scroll_offset().translated(delta_x, delta_y));
}

void PaintableBox::scroll_into_view(CSSPixelRect rect)
{
    auto scrollport = absolute_padding_box_rect();
    auto current_offset = scroll_offset();

    // Both rect and scrollport are in layout coordinate space (not scroll-adjusted).
    auto content_rect = rect.translated(-scrollport.x(), -scrollport.y());
    auto new_offset = current_offset;

    if (content_rect.right() > current_offset.x() + scrollport.width())
        new_offset.set_x(content_rect.right() - scrollport.width());
    else if (content_rect.left() < current_offset.x())
        new_offset.set_x(content_rect.left());

    if (content_rect.bottom() > current_offset.y() + scrollport.height())
        new_offset.set_y(content_rect.bottom() - scrollport.height());
    else if (content_rect.top() < current_offset.y())
        new_offset.set_y(content_rect.top());

    set_scroll_offset(new_offset);
}

void PaintableBox::set_offset(CSSPixelPoint offset)
{
    m_offset = offset;
}

void PaintableBox::set_content_size(CSSPixelSize size)
{
    m_content_size = size;
    if (auto layout_box = as_if<Layout::Box>(layout_node()))
        layout_box->did_set_content_size();
}

CSSPixelPoint PaintableBox::offset() const
{
    return m_offset;
}

CSSPixelRect PaintableBox::compute_absolute_rect() const
{
    CSSPixelRect rect { offset(), content_size() };
    for (auto const* block = containing_block(); block; block = block->containing_block())
        rect.translate_by(block->offset());
    return rect;
}

CSSPixelRect PaintableBox::absolute_rect() const
{
    if (!m_absolute_rect.has_value())
        m_absolute_rect = compute_absolute_rect();
    return *m_absolute_rect;
}

CSSPixelRect PaintableBox::absolute_padding_box_rect() const
{
    if (!m_absolute_padding_box_rect.has_value()) {
        auto absolute_rect = this->absolute_rect();
        CSSPixelRect rect;
        rect.set_x(absolute_rect.x() - box_model().padding.left);
        rect.set_width(content_width() + box_model().padding.left + box_model().padding.right);
        rect.set_y(absolute_rect.y() - box_model().padding.top);
        rect.set_height(content_height() + box_model().padding.top + box_model().padding.bottom);
        m_absolute_padding_box_rect = rect;
    }
    return *m_absolute_padding_box_rect;
}

Optional<CSSPixelRect> PaintableBox::absolute_resizer_rect(ChromeMetrics const& metrics) const
{
    if (!has_resizer())
        return {};
    auto padding_rect = absolute_padding_box_rect();
    CSSPixels x = is_chrome_mirrored() ? padding_rect.x() : padding_rect.right() - metrics.resize_gripper_size;
    CSSPixels y = padding_rect.bottom() - metrics.resize_gripper_size;
    return CSSPixelRect { x, y, metrics.resize_gripper_size, metrics.resize_gripper_size };
}

CSSPixelRect PaintableBox::absolute_border_box_rect() const
{
    if (!m_absolute_border_box_rect.has_value()) {
        auto padded_rect = this->absolute_padding_box_rect();
        CSSPixelRect rect;
        auto use_collapsing_borders_model = override_borders_data().has_value();
        // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
        auto border_top = use_collapsing_borders_model ? round(box_model().border.top / 2) : box_model().border.top;
        auto border_bottom = use_collapsing_borders_model ? round(box_model().border.bottom / 2) : box_model().border.bottom;
        auto border_left = use_collapsing_borders_model ? round(box_model().border.left / 2) : box_model().border.left;
        auto border_right = use_collapsing_borders_model ? round(box_model().border.right / 2) : box_model().border.right;
        rect.set_x(padded_rect.x() - border_left);
        rect.set_width(padded_rect.width() + border_left + border_right);
        rect.set_y(padded_rect.y() - border_top);
        rect.set_height(padded_rect.height() + border_top + border_bottom);
        m_absolute_border_box_rect = rect;
    }
    return *m_absolute_border_box_rect;
}

// https://drafts.csswg.org/css-overflow-4/#overflow-clip-edge
CSSPixelRect PaintableBox::overflow_clip_edge_rect() const
{
    // https://drafts.csswg.org/css-overflow-4/#overflow-clip-margin
    // Values are defined as follows:
    // '<visual-box>'
    //     Specifies the box edge to use as the overflow clip edge origin, i.e. when the specified offset is zero.
    //     If omitted, defaults to 'padding-box' on non-replaced elements, or 'content-box' on replaced elements.
    // FIXME: We can't parse this yet so it's always omitted for now.
    auto overflow_clip_edge = absolute_padding_box_rect();
    if (layout_node().is_replaced_box()) {
        overflow_clip_edge = absolute_rect();
    }

    // '<length [0,∞]>'
    //     The specified offset dictates how much the overflow clip edge is expanded from the specified box edge
    //     Negative values are invalid. Defaults to zero if omitted.
    overflow_clip_edge.inflate(
        computed_values().overflow_clip_margin().top().length().absolute_length_to_px(),
        computed_values().overflow_clip_margin().right().length().absolute_length_to_px(),
        computed_values().overflow_clip_margin().bottom().length().absolute_length_to_px(),
        computed_values().overflow_clip_margin().left().length().absolute_length_to_px());
    return overflow_clip_edge;
}

template<typename Callable>
static CSSPixelRect united_rect_for_continuation_chain(PaintableBox const& start, Callable get_rect)
{
    // Combine the absolute rects of all paintable boxes of all nodes in the continuation chain. Without this, we
    // calculate the wrong rect for inline nodes that were split because of block elements.
    Optional<CSSPixelRect> result;

    // FIXME: instead of walking the continuation chain in the layout tree, also keep track of this chain in the
    //        painting tree so we can skip visiting the layout nodes altogether.
    for (auto const* node = &start.layout_node_with_style_and_box_metrics(); node; node = node->continuation_of_node()) {
        for (auto const& paintable : node->paintables()) {
            if (!is<PaintableBox>(paintable))
                continue;
            auto const& paintable_box = static_cast<PaintableBox const&>(paintable);
            auto paintable_border_box_rect = get_rect(paintable_box);
            if (!result.has_value())
                result = paintable_border_box_rect;
            else if (!paintable_border_box_rect.is_empty())
                result->unite(paintable_border_box_rect);
        }
    }
    return result.value_or({});
}

CSSPixelRect PaintableBox::absolute_united_border_box_rect() const
{
    return united_rect_for_continuation_chain(*this, [](auto const& paintable_box) {
        return paintable_box.absolute_border_box_rect();
    });
}

CSSPixelRect PaintableBox::absolute_united_content_rect() const
{
    return united_rect_for_continuation_chain(*this, [](auto const& paintable_box) {
        return paintable_box.absolute_rect();
    });
}

CSSPixelRect PaintableBox::absolute_united_padding_box_rect() const
{
    return united_rect_for_continuation_chain(*this, [](auto const& paintable_box) {
        return paintable_box.absolute_padding_box_rect();
    });
}

Optional<CSSPixelRect> PaintableBox::get_clip_rect() const
{
    auto clip = computed_values().clip();
    if (clip.is_rect() && layout_node_with_style_and_box_metrics().is_absolutely_positioned()) {
        auto border_box = absolute_border_box_rect();
        return clip.to_rect().resolved(layout_node(), border_box);
    }
    return {};
}

bool PaintableBox::wants_mouse_events() const
{
    return (m_own_scroll_frame && could_be_scrolled_by_wheel_event()) || has_resizer();
}

bool PaintableBox::could_be_scrolled_by_wheel_event(ScrollDirection direction) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    Gfx::Orientation orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto overflow = is_horizontal ? computed_values().overflow_x() : computed_values().overflow_y();

    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return false;

    CSSPixels scrollable_overflow_size = scrollable_overflow_rect->primary_size_for_orientation(orientation);
    CSSPixels scrollport_size = absolute_padding_box_rect().primary_size_for_orientation(orientation);

    bool overflow_value_allows_scrolling = overflow == CSS::Overflow::Auto || overflow == CSS::Overflow::Scroll;
    if ((is_viewport_paintable() && overflow != CSS::Overflow::Hidden) || overflow_value_allows_scrolling)
        return scrollable_overflow_size > scrollport_size;

    return false;
}

bool PaintableBox::could_be_scrolled_by_wheel_event() const
{
    return could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal) || could_be_scrolled_by_wheel_event(ScrollDirection::Vertical);
}

bool PaintableBox::overflow_property_applies() const
{
    // https://drafts.csswg.org/css-overflow-3/#overflow-control
    // Overflow properties apply to block containers, flex containers and grid containers.
    // FIXME: Ideally we would check whether overflow applies positively rather than listing exceptions. However,
    //        not all elements that should support overflow are currently identifiable that way.
    if (is<SVGPaintable>(*this))
        return false;
    auto const& display = computed_values().display();
    if (layout_node().is_inline_node())
        return false;
    if (display.is_ruby_inside())
        return false;
    if (display.is_internal() && !display.is_table_cell() && !display.is_table_caption())
        return false;
    return true;
}

CSSPixels PaintableBox::available_scrollbar_length(ScrollDirection direction, ChromeMetrics const& metrics) const

{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto padding_rect = absolute_padding_box_rect();
    CSSPixels full_scrollport_length = is_horizontal ? padding_rect.width() : padding_rect.height();
    if (has_resizer())
        full_scrollport_length -= metrics.resize_gripper_size;
    else {
        if (is_horizontal && could_be_scrolled_by_wheel_event(ScrollDirection::Vertical))
            full_scrollport_length -= metrics.scroll_gutter_thickness;
        if (!is_horizontal && could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal))
            full_scrollport_length -= metrics.scroll_gutter_thickness;
    }
    return full_scrollport_length;
}

Optional<CSSPixelRect> PaintableBox::absolute_scrollbar_rect(ScrollDirection direction, bool with_gutter, ChromeMetrics const& metrics) const
{
    if (!could_be_scrolled_by_wheel_event(direction))
        return {};

    if (computed_values().scrollbar_width() == CSS::ScrollbarWidth::None)
        return {};

    bool is_horizontal = direction == ScrollDirection::Horizontal;
    bool adjusting_for_resizer = has_resizer();

    CSSPixels rect_thickness = with_gutter
        ? metrics.scroll_gutter_thickness
        : metrics.scroll_thumb_thickness_thin + metrics.scroll_thumb_padding_thin;
    CSSPixelRect scrollbar_rect = absolute_padding_box_rect();

    if (is_horizontal) {
        if (!adjusting_for_resizer && could_be_scrolled_by_wheel_event(ScrollDirection::Vertical)) {
            scrollbar_rect.set_width(max(CSSPixels { 0 }, scrollbar_rect.width() - metrics.scroll_gutter_thickness));
            if (is_chrome_mirrored())
                scrollbar_rect.set_x(scrollbar_rect.x() + metrics.scroll_gutter_thickness);
        } else if (adjusting_for_resizer) {
            scrollbar_rect.set_width(available_scrollbar_length(ScrollDirection::Horizontal, metrics));
            if (is_chrome_mirrored())
                scrollbar_rect.set_x(scrollbar_rect.x() + metrics.resize_gripper_size);
        }
        scrollbar_rect.set_y(max(CSSPixels { 0 }, scrollbar_rect.bottom() - rect_thickness));
        scrollbar_rect.set_height(rect_thickness);
    } else {
        if (adjusting_for_resizer)
            scrollbar_rect.set_height(available_scrollbar_length(ScrollDirection::Vertical, metrics));
        if (!is_chrome_mirrored())
            scrollbar_rect.set_x(max(CSSPixels { 0 }, scrollbar_rect.right() - rect_thickness));
        scrollbar_rect.set_width(rect_thickness);
    }
    return scrollbar_rect;
}

Optional<PaintableBox::ScrollbarData> PaintableBox::compute_scrollbar_data(ScrollDirection direction, ChromeMetrics const& metrics, ScrollStateSnapshot const* scroll_state_snapshot) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto overflow = is_horizontal ? computed_values().overflow_x() : computed_values().overflow_y();

    if (overflow != CSS::Overflow::Scroll && !could_be_scrolled_by_wheel_event(direction))
        return {};

    if (!own_scroll_frame_id().has_value())
        return {};

    CSSPixelRect scrollable_overflow_rect = this->scrollable_overflow_rect().value();
    CSSPixels scrollable_overflow_length = scrollable_overflow_rect.primary_size_for_orientation(orientation);
    if (scrollable_overflow_length == 0)
        return {};

    bool with_gutter = is_horizontal ? m_draw_enlarged_horizontal_scrollbar : m_draw_enlarged_vertical_scrollbar;
    auto scrollbar_rect = absolute_scrollbar_rect(direction, with_gutter, metrics);
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
    CSSPixels scrollport_size = absolute_padding_box_rect().primary_size_for_orientation(orientation);
    CSSPixels min_thumb_length = min(usable_scrollbar_length, metrics.scroll_thumb_min_length);
    CSSPixels thumb_length = max(usable_scrollbar_length * (scrollport_size / scrollable_overflow_length), min_thumb_length);

    ScrollbarData scrollbar_data = { .gutter_rect = {}, .thumb_rect = scrollbar_rect.value(), .thumb_travel_to_scroll_ratio = 0 };

    scrollbar_data.thumb_rect.set_primary_size_for_orientation(orientation, thumb_length);
    scrollbar_data.thumb_rect.set_secondary_size_for_orientation(orientation, thumb_thickness);
    scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_margin);
    if (with_gutter || (!is_horizontal && is_chrome_mirrored()))
        scrollbar_data.thumb_rect.translate_secondary_offset_for_orientation(orientation, thumb_margin);
    if (with_gutter)
        scrollbar_data.gutter_rect = scrollbar_rect.value();
    if (scrollable_overflow_length > scrollport_size)
        scrollbar_data.thumb_travel_to_scroll_ratio = (usable_scrollbar_length - thumb_length) / (scrollable_overflow_length - scrollport_size);

    if (scroll_state_snapshot) {
        auto own_offset = scroll_state_snapshot->device_offset_for_frame_with_id(own_scroll_frame_id().value());
        auto device_scroll_offset = is_horizontal ? -own_offset.x() : -own_offset.y();
        auto device_pixels_per_css_pixel = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
        CSSPixels thumb_offset = CSSPixels::nearest_value_for(device_scroll_offset / device_pixels_per_css_pixel) * scrollbar_data.thumb_travel_to_scroll_ratio;
        scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_offset);
    }

    return scrollbar_data;
}

void PaintableBox::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    auto empty_cells_property_applies = [this]() {
        return display().is_internal_table() && computed_values().empty_cells() == CSS::EmptyCells::Hide && !has_children();
    };

    if (phase == PaintPhase::Background && !empty_cells_property_applies()) {
        paint_backdrop_filter(context);
        paint_background(context);
        paint_box_shadow(context);
    }

    auto const is_table_with_collapsed_borders = display().is_table_inside() && computed_values().border_collapse() == CSS::BorderCollapse::Collapse;
    if (!display().is_table_cell() && !is_table_with_collapsed_borders && phase == PaintPhase::Border) {
        paint_border(context);
    }

    if ((display().is_table_inside() || computed_values().border_collapse() == CSS::BorderCollapse::Collapse) && phase == PaintPhase::TableCollapsedBorder) {
        paint_table_borders(context, *this);
    }

    if (phase == PaintPhase::Outline) {
        auto const& outline_data = this->outline_data();
        if (outline_data.has_value()) {
            auto outline_offset = this->outline_offset();
            auto border_radius_data = normalized_border_radii_data(ShrinkRadiiForBorders::No);
            auto borders_rect = absolute_border_box_rect();

            auto outline_offset_x = outline_offset;
            auto outline_offset_y = outline_offset;
            // "Both the height and the width of the outside of the shape drawn by the outline should not
            // become smaller than twice the computed value of the outline-width property to make sure
            // that an outline can be rendered even with large negative values."
            // https://www.w3.org/TR/css-ui-4/#outline-offset
            // So, if the horizontal outline offset is > half the borders_rect's width then we set it to that.
            // (And the same for y)
            if ((borders_rect.width() / 2) + outline_offset_x < 0)
                outline_offset_x = -borders_rect.width() / 2;
            if ((borders_rect.height() / 2) + outline_offset_y < 0)
                outline_offset_y = -borders_rect.height() / 2;

            border_radius_data.inflate(outline_data->top.width + outline_offset_y, outline_data->right.width + outline_offset_x, outline_data->bottom.width + outline_offset_y, outline_data->left.width + outline_offset_x);
            borders_rect.inflate(outline_data->top.width + outline_offset_y, outline_data->right.width + outline_offset_x, outline_data->bottom.width + outline_offset_y, outline_data->left.width + outline_offset_x);

            paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(borders_rect), border_radius_data.as_corners(context.device_pixel_converter()), outline_data->to_device_pixels(context));
        }
    }

    if (phase == PaintPhase::Overlay) {
        ChromeMetrics const& metrics = context.chrome_metrics();

        if ((g_paint_viewport_scrollbars || !is_viewport_paintable())
            && computed_values().scrollbar_width() != CSS::ScrollbarWidth::None) {
            auto scrollbar_colors = computed_values().scrollbar_color();

            for (auto direction : { ScrollDirection::Vertical, ScrollDirection::Horizontal }) {
                auto scrollbar_data = compute_scrollbar_data(direction, metrics);
                if (!scrollbar_data.has_value())
                    continue;
                context.display_list_recorder().paint_scrollbar(
                    own_scroll_frame_id().value(),
                    context.rounded_device_rect(scrollbar_data->gutter_rect).to_type<int>(),
                    context.rounded_device_rect(scrollbar_data->thumb_rect).to_type<int>(),
                    scrollbar_data->thumb_travel_to_scroll_ratio.to_double(),
                    scrollbar_colors.thumb_color,
                    scrollbar_colors.track_color,
                    direction == ScrollDirection::Vertical);
            }
        }
        if (auto resizer_rect = absolute_resizer_rect(metrics); resizer_rect.has_value()) {
            bool bottom_left_resizer = is_chrome_mirrored();
            CSSPixels padding = metrics.resize_gripper_padding;
            CSSPixelRect css_rect = resizer_rect.value()
                                        .shrunken(padding, padding)
                                        .translated(bottom_left_resizer ? padding / 2 : -padding / 2, -padding / 2);
            Gfx::IntRect rect = context.rounded_device_rect(css_rect).to_type<int>();
            Gfx::Color dark { 0, 0, 0, 100 };
            Gfx::Color light { 255, 255, 255, 100 };
            auto& recorder = context.display_list_recorder();
            auto paint_resizer_line = [&](int step, Gfx::Color color) {
                Gfx::IntPoint from = { bottom_left_resizer ? rect.left() + step : rect.right() - step, rect.bottom() };
                Gfx::IntPoint to = { bottom_left_resizer ? rect.left() : rect.right(), rect.bottom() - step };
                recorder.draw_line(from, to, color, 1, Gfx::LineStyle::Solid);
            };
            for (int step = (rect.width() / 3) - 1; step < rect.width(); step += rect.width() / 3) {
                paint_resizer_line(step, light);
                paint_resizer_line(step + 1, dark);
            }
        }
    }
}

void PaintableBox::paint_inspector_overlay_internal(DisplayListRecordingContext& context) const
{
    auto content_rect = absolute_united_content_rect();
    auto margin_rect = united_rect_for_continuation_chain(*this, [](PaintableBox const& box) {
        auto margin_box = box.box_model().margin_box();
        return CSSPixelRect {
            box.absolute_x() - margin_box.left,
            box.absolute_y() - margin_box.top,
            box.content_width() + margin_box.left + margin_box.right,
            box.content_height() + margin_box.top + margin_box.bottom,
        };
    });
    auto border_rect = absolute_united_border_box_rect();
    auto padding_rect = absolute_united_padding_box_rect();

    auto paint_inspector_rect = [&](CSSPixelRect const& rect, Color color) {
        auto device_rect = context.enclosing_device_rect(rect).to_type<int>();
        context.display_list_recorder().fill_rect(device_rect, color.with_alpha(100));
        context.display_list_recorder().draw_rect(device_rect, color);
    };

    paint_inspector_rect(margin_rect, Color::Yellow);
    paint_inspector_rect(padding_rect, Color::Cyan);
    paint_inspector_rect(border_rect, Color::Green);
    paint_inspector_rect(content_rect, Color::Magenta);

    auto font = Platform::FontPlugin::the().default_font(12);

    StringBuilder builder(StringBuilder::Mode::UTF16);
    builder.append(debug_description());
    builder.appendff(" {}x{} @ {},{}", border_rect.width(), border_rect.height(), border_rect.x(), border_rect.y());
    auto size_text = builder.to_utf16_string();
    auto size_text_rect = border_rect;
    size_text_rect.set_y(border_rect.y() + border_rect.height());
    size_text_rect.set_top(size_text_rect.top());
    size_text_rect.set_width(CSSPixels::nearest_value_for(font->width(size_text)) + 4);
    size_text_rect.set_height(CSSPixels::nearest_value_for(font->pixel_size()) + 4);
    auto size_text_device_rect = context.enclosing_device_rect(size_text_rect).to_type<int>();
    context.display_list_recorder().fill_rect(size_text_device_rect, context.palette().color(Gfx::ColorRole::Tooltip));
    context.display_list_recorder().draw_rect(size_text_device_rect, context.palette().threed_shadow1());
    context.display_list_recorder().draw_text(size_text_device_rect, size_text, font->with_size(font->point_size() * context.device_pixels_per_css_pixel()), Gfx::TextAlignment::Center, context.palette().color(Gfx::ColorRole::TooltipText));
}

void PaintableBox::set_stacking_context(GC::Ref<StackingContext> stacking_context)
{
    m_stacking_context = move(stacking_context);
}

void PaintableBox::invalidate_stacking_context()
{
    m_stacking_context = nullptr;
}

BordersData PaintableBox::remove_element_kind_from_borders_data(PaintableBox::BordersDataWithElementKind borders_data)
{
    return {
        .top = borders_data.top.border_data,
        .right = borders_data.right.border_data,
        .bottom = borders_data.bottom.border_data,
        .left = borders_data.left.border_data,
    };
}

void PaintableBox::paint_border(DisplayListRecordingContext& context) const
{
    auto borders_data = m_override_borders_data.has_value() ? remove_element_kind_from_borders_data(m_override_borders_data.value()) : BordersData {
        .top = box_model().border.top == 0 ? CSS::BorderData() : computed_values().border_top(),
        .right = box_model().border.right == 0 ? CSS::BorderData() : computed_values().border_right(),
        .bottom = box_model().border.bottom == 0 ? CSS::BorderData() : computed_values().border_bottom(),
        .left = box_model().border.left == 0 ? CSS::BorderData() : computed_values().border_left(),
    };
    paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(absolute_border_box_rect()), normalized_border_radii_data().as_corners(context.device_pixel_converter()), borders_data.to_device_pixels(context));
}

void PaintableBox::paint_backdrop_filter(DisplayListRecordingContext& context) const
{
    if (!m_backdrop_filter.has_filters())
        return;

    auto backdrop_region = context.rounded_device_rect(absolute_border_box_rect());
    auto border_radii_data = normalized_border_radii_data();
    ScopedCornerRadiusClip corner_clipper { context, backdrop_region, border_radii_data };
    if (auto resolved_backdrop_filter = to_gfx_filter(m_backdrop_filter, context.device_pixels_per_css_pixel()); resolved_backdrop_filter.has_value())
        context.display_list_recorder().apply_backdrop_filter(backdrop_region.to_type<int>(), border_radii_data.as_corners(context.device_pixel_converter()), *resolved_backdrop_filter);
}

void PaintableBox::paint_background(DisplayListRecordingContext& context) const
{
    // If the body's background properties were propagated to the root element, do not re-paint the body's background.
    if (layout_node_with_style_and_box_metrics().is_body() && document().html_element()->should_use_body_background_properties())
        return;

    // If the body's background was propagated to the root element, use the body's image-rendering value.
    auto image_rendering = computed_values().image_rendering();
    if (layout_node().is_root_element()
        && document().html_element()
        && document().html_element()->should_use_body_background_properties()) {
        image_rendering = document().background_image_rendering();
    }

    Painting::paint_background(context, *this, image_rendering, m_resolved_background, normalized_border_radii_data());
}

void PaintableBox::paint_box_shadow(DisplayListRecordingContext& context) const
{
    auto const& resolved_box_shadow_data = box_shadow_data();
    if (resolved_box_shadow_data.is_empty())
        return;
    auto borders_data = BordersData {
        .top = computed_values().border_top(),
        .right = computed_values().border_right(),
        .bottom = computed_values().border_bottom(),
        .left = computed_values().border_left(),
    };
    Painting::paint_box_shadow(context, absolute_border_box_rect(), absolute_padding_box_rect(),
        borders_data, normalized_border_radii_data(), resolved_box_shadow_data);
}

BorderRadiiData PaintableBox::normalized_border_radii_data(ShrinkRadiiForBorders shrink) const
{
    auto border_radii_data = this->border_radii_data();
    if (shrink == ShrinkRadiiForBorders::Yes)
        border_radii_data.shrink(computed_values().border_top().width, computed_values().border_right().width, computed_values().border_bottom().width, computed_values().border_left().width);
    return border_radii_data;
}

Optional<int> PaintableBox::own_scroll_frame_id() const
{
    if (m_own_scroll_frame)
        return m_own_scroll_frame->id();
    return {};
}

Optional<int> PaintableBox::scroll_frame_id() const
{
    if (m_enclosing_scroll_frame)
        return m_enclosing_scroll_frame->id();
    return {};
}

CSSPixelPoint PaintableBox::transform_to_local_coordinates(CSSPixelPoint screen_position) const
{
    if (!accumulated_visual_context())
        return screen_position;

    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& scroll_state = document().paintable()->scroll_state_snapshot();
    auto result = accumulated_visual_context()->transform_point_for_hit_test(screen_position.to_type<float>() * pixel_ratio, scroll_state);
    if (!result.has_value())
        return screen_position;
    return (*result / pixel_ratio).to_type<CSSPixels>();
}

bool PaintableBox::has_resizer() const
{
    // https://drafts.csswg.org/css-ui#resize
    if (is_viewport_paintable())
        return false;

    // The effect of the resize property on generated content is undefined.
    // Implementations should not apply the resize property to generated content.

    if (layout_node().generated_for_pseudo_element().has_value())
        return false;

    auto axes = compute_physical_resize_axes(computed_values());
    return axes.horizontal || axes.vertical;
}

bool PaintableBox::is_chrome_mirrored() const
{
    auto const& writing_mode = computed_values().writing_mode();
    return (writing_mode == CSS::WritingMode::HorizontalTb && computed_values().direction() == CSS::Direction::Rtl)
        || writing_mode == CSS::WritingMode::VerticalRl
        || writing_mode == CSS::WritingMode::SidewaysRl;
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mousedown(Badge<EventHandler>, CSSPixelPoint position, unsigned, unsigned)
{
    position = transform_to_local_coordinates(position);
    ChromeMetrics metrics = document().page().chrome_metrics();

    if (resizer_contains(position, metrics)) {
        if (auto* element = as_if<DOM::Element>(dom_node().ptr())) {
            navigable()->event_handler().set_element_resize_in_progress(*element, position);
            return Paintable::DispatchEventOfSameName::No;
        }
    }

    auto handle_scrollbar = [&](auto direction) {
        auto scrollbar_data = compute_scrollbar_data(direction, metrics);
        if (!scrollbar_data.has_value())
            return false;

        if (scrollbar_data->gutter_rect.contains(position)) {
            m_scroll_thumb_dragging_direction = direction;

            navigable()->event_handler().set_mouse_event_tracking_paintable(this);
            scroll_to_mouse_position(position, metrics);
            return true;
        }

        return false;
    };

    if (handle_scrollbar(ScrollDirection::Vertical))
        return Paintable::DispatchEventOfSameName::No;
    if (handle_scrollbar(ScrollDirection::Horizontal))
        return Paintable::DispatchEventOfSameName::No;

    return Paintable::DispatchEventOfSameName::Yes;
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned)
{
    if (m_scroll_thumb_grab_position.has_value()) {
        m_scroll_thumb_grab_position.clear();
        m_scroll_thumb_dragging_direction.clear();
        navigable()->event_handler().set_mouse_event_tracking_paintable(nullptr);
    }
    return Paintable::DispatchEventOfSameName::Yes;
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mousemove(Badge<EventHandler>, CSSPixelPoint position, unsigned, unsigned)
{
    position = transform_to_local_coordinates(position);
    ChromeMetrics metrics = document().page().chrome_metrics();

    if (m_scroll_thumb_grab_position.has_value()) {
        scroll_to_mouse_position(position, metrics);
        return Paintable::DispatchEventOfSameName::No;
    }

    auto previous_draw_enlarged_horizontal_scrollbar = m_draw_enlarged_horizontal_scrollbar;
    m_draw_enlarged_horizontal_scrollbar = scrollbar_contains(ScrollDirection::Horizontal, position, metrics);
    if (previous_draw_enlarged_horizontal_scrollbar != m_draw_enlarged_horizontal_scrollbar)
        set_needs_display();

    auto previous_draw_enlarged_vertical_scrollbar = m_draw_enlarged_vertical_scrollbar;
    m_draw_enlarged_vertical_scrollbar = scrollbar_contains(ScrollDirection::Vertical, position, metrics);
    if (previous_draw_enlarged_vertical_scrollbar != m_draw_enlarged_vertical_scrollbar)
        set_needs_display();

    if (m_draw_enlarged_horizontal_scrollbar || m_draw_enlarged_vertical_scrollbar)
        return Paintable::DispatchEventOfSameName::No;

    return Paintable::DispatchEventOfSameName::Yes;
}

void PaintableBox::handle_mouseleave(Badge<EventHandler>)
{
    // FIXME: early return needed as MacOSX calls this even when user is pressing mouse button
    // https://github.com/LadybirdBrowser/ladybird/issues/5844
    if (m_scroll_thumb_dragging_direction.has_value())
        return;

    auto previous_draw_enlarged_horizontal_scrollbar = m_draw_enlarged_horizontal_scrollbar;
    m_draw_enlarged_horizontal_scrollbar = false;
    if (previous_draw_enlarged_horizontal_scrollbar != m_draw_enlarged_horizontal_scrollbar)
        set_needs_display();

    auto previous_draw_enlarged_vertical_scrollbar = m_draw_enlarged_vertical_scrollbar;
    m_draw_enlarged_vertical_scrollbar = false;
    if (previous_draw_enlarged_vertical_scrollbar != m_draw_enlarged_vertical_scrollbar)
        set_needs_display();
}

bool PaintableBox::scrollbar_contains(ScrollDirection direction, CSSPixelPoint adjusted_position, ChromeMetrics const& metrics) const
{
    bool with_gutter = direction == ScrollDirection::Horizontal ? m_draw_enlarged_horizontal_scrollbar : m_draw_enlarged_vertical_scrollbar;
    if (auto rect = absolute_scrollbar_rect(direction, with_gutter, metrics); rect.has_value())
        return rect->contains(adjusted_position);
    return false;
}

void PaintableBox::scroll_to_mouse_position(CSSPixelPoint position, ChromeMetrics const& metrics)
{
    VERIFY(m_scroll_thumb_dragging_direction.has_value());

    auto const& scroll_state = document().paintable()->scroll_state_snapshot();
    auto scrollbar_data = compute_scrollbar_data(m_scroll_thumb_dragging_direction.value(), metrics, &scroll_state);
    VERIFY(scrollbar_data.has_value());

    auto orientation = m_scroll_thumb_dragging_direction == ScrollDirection::Horizontal ? Orientation::Horizontal : Orientation::Vertical;
    auto offset_relative_to_gutter = (position - scrollbar_data->gutter_rect.location()).primary_offset_for_orientation(orientation);
    auto gutter_size = scrollbar_data->gutter_rect.primary_size_for_orientation(orientation);
    auto thumb_size = scrollbar_data->thumb_rect.primary_size_for_orientation(orientation);

    // Set the thumb grab position, if we haven't got one already.
    if (!m_scroll_thumb_grab_position.has_value()) {
        m_scroll_thumb_grab_position = scrollbar_data->thumb_rect.contains(position)
            ? (position - scrollbar_data->thumb_rect.location()).primary_offset_for_orientation(orientation)
            : max(min(offset_relative_to_gutter, thumb_size / 2), offset_relative_to_gutter - gutter_size + thumb_size);
    }

    // Calculate the relative scroll position (0..1) based on the position of the mouse cursor. We only move the thumb
    // if we are interacting with the grab point on the thumb. E.g. if the thumb is all the way to its minimum position
    // and the position is beyond the grab point, we should do nothing.
    auto constrained_offset = AK::clamp(offset_relative_to_gutter - m_scroll_thumb_grab_position.value(), 0, gutter_size - thumb_size);
    auto scroll_position = constrained_offset.to_double() / (gutter_size - thumb_size).to_double();

    // Calculate the scroll offset we need to apply to the viewport or element.
    auto scrollable_overflow_size = scrollable_overflow_rect()->primary_size_for_orientation(orientation);
    auto padding_size = absolute_padding_box_rect().primary_size_for_orientation(orientation);
    auto scroll_position_in_pixels = CSSPixels::nearest_value_for(scroll_position * (scrollable_overflow_size - padding_size));

    // Set the new scroll offset.
    auto new_scroll_offset = scroll_offset();
    new_scroll_offset.set_primary_offset_for_orientation(orientation, scroll_position_in_pixels);
    set_scroll_offset(new_scroll_offset);
}

bool PaintableBox::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int wheel_delta_x, int wheel_delta_y)
{
    // if none of the axes we scrolled with can be accepted by this element, don't handle scroll.
    if ((!wheel_delta_x || !could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal)) && (!wheel_delta_y || !could_be_scrolled_by_wheel_event(ScrollDirection::Vertical))) {
        return false;
    }

    auto scroll_handled = scroll_by(wheel_delta_x, wheel_delta_y);
    return scroll_handled == ScrollHandled::Yes;
}

TraversalDecision PaintableBox::hit_test_chrome(CSSPixelPoint adjusted_position, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    // FIXME: This const_cast is not great, but this method is invoked from overrides of virtual const methods.
    HitTestResult result { const_cast<PaintableBox&>(*this), 0, {}, {}, CSS::CursorPredefined::Default };
    ChromeMetrics metrics = document().page().chrome_metrics();

    if (resizer_contains(adjusted_position, metrics)) {
        auto axes = compute_physical_resize_axes(computed_values());

        if (axes.vertical) {
            if (axes.horizontal) {
                if (is_chrome_mirrored())
                    result.cursor_override = CSS::CursorPredefined::SwResize;
                else
                    result.cursor_override = CSS::CursorPredefined::SeResize;
            } else {
                result.cursor_override = CSS::CursorPredefined::NsResize;
            }
        } else {
            result.cursor_override = CSS::CursorPredefined::EwResize;
        }
        return callback(result);
    }
    if (scrollbar_contains(ScrollDirection::Horizontal, adjusted_position, metrics))
        return callback(result);

    if (m_draw_enlarged_horizontal_scrollbar) {
        m_draw_enlarged_horizontal_scrollbar = false;
        result.paintable->set_needs_display();
    }
    if (scrollbar_contains(ScrollDirection::Vertical, adjusted_position, metrics))
        return callback(result);

    if (m_draw_enlarged_vertical_scrollbar) {
        m_draw_enlarged_vertical_scrollbar = false;
        result.paintable->set_needs_display();
    }

    return TraversalDecision::Continue;
}

bool PaintableBox::resizer_contains(CSSPixelPoint adjusted_position, ChromeMetrics const& metrics) const
{
    auto handle_rect = absolute_resizer_rect(metrics);
    if (!handle_rect.has_value())
        return false;
    bool bottom_left_resizer = is_chrome_mirrored();
    handle_rect->inflate(0, bottom_left_resizer ? 0 : box_model().border.right, box_model().border.bottom, bottom_left_resizer ? box_model().border.left : 0);

    return handle_rect->contains(adjusted_position);
}

TraversalDecision PaintableBox::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    auto const is_visible = computed_values().visibility() == CSS::Visibility::Visible;

    // Only hit test chrome (scrollbars, etc.) for visible elements.
    if (is_visible) {
        if (hit_test_chrome(position, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    if (is_viewport_paintable()) {
        auto& viewport_paintable = const_cast<ViewportPaintable&>(static_cast<ViewportPaintable const&>(*this));
        viewport_paintable.build_stacking_context_tree_if_needed();
        viewport_paintable.document().update_paint_and_hit_testing_properties_if_needed();
        viewport_paintable.refresh_scroll_state();
        return stacking_context()->hit_test(position, type, callback);
    }

    if (stacking_context())
        return TraversalDecision::Continue;

    if (hit_test_children(position, type, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    // Hidden elements and elements with pointer-events: none shouldn't be hit.
    if (!is_visible || !visible_for_hit_testing())
        return TraversalDecision::Continue;

    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& scroll_state = document().paintable()->scroll_state_snapshot();
    Optional<CSSPixelPoint> local_position;
    if (auto state = accumulated_visual_context()) {
        auto result = state->transform_point_for_hit_test(position.to_type<float>() * pixel_ratio, scroll_state);
        if (result.has_value())
            local_position = (*result / pixel_ratio).to_type<CSSPixels>();
    } else {
        local_position = position;
    }

    if (!local_position.has_value())
        return TraversalDecision::Continue;

    auto border_box_rect = absolute_border_box_rect();
    if (!border_box_rect.contains(local_position.value()))
        return TraversalDecision::Continue;

    if (auto radii = border_radii_data(); radii.has_any_radius()) {
        if (!radii.contains(local_position.value(), border_box_rect))
            return TraversalDecision::Continue;
    }

    if (hit_test_continuation(callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    return callback(HitTestResult { const_cast<PaintableBox&>(*this) });
}

TraversalDecision PaintableBox::hit_test_continuation(Function<TraversalDecision(HitTestResult)> const& callback) const
{
    // If we're hit testing the "middle" part of a continuation chain, we are dealing with an anonymous box that is
    // linked to a parent inline node. Since our block element children did not match the hit test, but we did, we
    // should walk the continuation chain up to the inline parent and return a hit on that instead.
    auto continuation_node = layout_node_with_style_and_box_metrics().continuation_of_node();
    if (!continuation_node || !layout_node().is_anonymous())
        return TraversalDecision::Continue;

    while (continuation_node->continuation_of_node())
        continuation_node = continuation_node->continuation_of_node();
    auto& paintable = *continuation_node->first_paintable();
    if (!paintable.visible_for_hit_testing())
        return TraversalDecision::Continue;

    return callback(HitTestResult { paintable });
}

Optional<HitTestResult> PaintableBox::hit_test(CSSPixelPoint position, HitTestType type) const
{
    Optional<HitTestResult> result;
    (void)PaintableBox::hit_test(position, type, [&](HitTestResult candidate) {
        if (!result.has_value()
            || candidate.vertical_distance.value_or(CSSPixels::max_integer_value) < result->vertical_distance.value_or(CSSPixels::max_integer_value)
            || candidate.horizontal_distance.value_or(CSSPixels::max_integer_value) < result->horizontal_distance.value_or(CSSPixels::max_integer_value)) {
            result = move(candidate);
        }

        if (result.has_value() && (type == HitTestType::Exact || (result->vertical_distance == 0 && result->horizontal_distance == 0)))
            return TraversalDecision::Break;
        return TraversalDecision::Continue;
    });
    return result;
}

TraversalDecision PaintableBox::hit_test_children(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    for (auto const* child = last_child(); child; child = child->previous_sibling()) {
        if (child->is_positioned() && child->computed_values().z_index().value_or(0) == 0)
            continue;
        if (child->has_stacking_context())
            continue;
        if (child->hit_test(position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }
    return TraversalDecision::Continue;
}

void PaintableBox::set_needs_display(InvalidateDisplayList should_invalidate_display_list)
{
    document().set_needs_display(absolute_rect(), should_invalidate_display_list);
}

// https://www.w3.org/TR/css-transforms-1/#reference-box
CSSPixelRect PaintableBox::transform_reference_box() const
{
    auto transform_box = computed_values().transform_box();
    // For SVG elements without associated CSS layout box, the used value for content-box is fill-box and for
    // border-box is stroke-box.
    // FIXME: This currently detects any SVG element except the <svg> one. Is that correct?
    //        And is it correct to use `else` below?
    if (is<Painting::SVGPaintable>(*this)) {
        switch (transform_box) {
        case CSS::TransformBox::ContentBox:
            transform_box = CSS::TransformBox::FillBox;
            break;
        case CSS::TransformBox::BorderBox:
            transform_box = CSS::TransformBox::StrokeBox;
            break;
        default:
            break;
        }
    }
    // For elements with associated CSS layout box, the used value for fill-box is content-box and for
    // stroke-box and view-box is border-box.
    else {
        switch (transform_box) {
        case CSS::TransformBox::FillBox:
            transform_box = CSS::TransformBox::ContentBox;
            break;
        case CSS::TransformBox::StrokeBox:
        case CSS::TransformBox::ViewBox:
            transform_box = CSS::TransformBox::BorderBox;
            break;
        default:
            break;
        }
    }

    switch (transform_box) {
    case CSS::TransformBox::ContentBox:
        // Uses the content box as reference box.
        // FIXME: The reference box of a table is the border box of its table wrapper box, not its table box.
        return absolute_rect();
    case CSS::TransformBox::BorderBox:
        // Uses the border box as reference box.
        // FIXME: The reference box of a table is the border box of its table wrapper box, not its table box.
        return absolute_border_box_rect();
    case CSS::TransformBox::FillBox:
        // Uses the object bounding box as reference box.
        // FIXME: For now we're using the content rect as an approximation.
        return absolute_rect();
    case CSS::TransformBox::StrokeBox:
        // Uses the stroke bounding box as reference box.
        // FIXME: For now we're using the border rect as an approximation.
        return absolute_border_box_rect();
    case CSS::TransformBox::ViewBox:
        // Uses the nearest SVG viewport as reference box.
        // FIXME: If a viewBox attribute is specified for the SVG viewport creating element:
        //  - The reference box is positioned at the origin of the coordinate system established by the viewBox attribute.
        //  - The dimension of the reference box is set to the width and height values of the viewBox attribute.
        auto* svg_paintable = first_ancestor_of_type<Painting::SVGSVGPaintable>();
        if (!svg_paintable)
            return absolute_border_box_rect();
        return svg_paintable->absolute_rect();
    }
    VERIFY_NOT_REACHED();
}

void PaintableBox::resolve_paint_properties()
{
    Base::resolve_paint_properties();

    auto const& computed_values = this->computed_values();
    auto const& layout_node = this->layout_node();

    // Border radii
    BorderRadiiData radii_data {};
    if (computed_values.has_noninitial_border_radii()) {
        CSSPixelRect const border_rect { 0, 0, border_box_width(), border_box_height() };

        auto const& border_top_left_radius = computed_values.border_top_left_radius();
        auto const& border_top_right_radius = computed_values.border_top_right_radius();
        auto const& border_bottom_right_radius = computed_values.border_bottom_right_radius();
        auto const& border_bottom_left_radius = computed_values.border_bottom_left_radius();

        radii_data = normalize_border_radii_data(layout_node, border_rect, border_top_left_radius,
            border_top_right_radius, border_bottom_right_radius,
            border_bottom_left_radius);
    }
    set_border_radii_data(radii_data);

    // Box shadows
    auto const& box_shadow_data = computed_values.box_shadow();
    Vector<Painting::ShadowData> resolved_box_shadow_data;
    resolved_box_shadow_data.ensure_capacity(box_shadow_data.size());
    for (auto const& layer : box_shadow_data)
        resolved_box_shadow_data.unchecked_append(ShadowData::from_css(layer, layout_node));
    set_box_shadow_data(move(resolved_box_shadow_data));

    // Outlines
    auto outline_data = borders_data_for_outline(layout_node, computed_values.outline_color(), computed_values.outline_style(), computed_values.outline_width());
    auto outline_offset = computed_values.outline_offset().to_px(layout_node);
    set_outline_data(outline_data);
    set_outline_offset(outline_offset);

    CSSPixelRect background_rect;
    Color background_color = computed_values.background_color();
    auto const* background_layers = &computed_values.background_layers();

    // https://drafts.csswg.org/css-backgrounds/#root-background
    // The background of the root element becomes the canvas background and its background painting area extends to
    // cover the entire canvas. However, any images are sized and positioned relative to the root element’s box as if
    // they were painted for that element alone.
    auto is_root = layout_node_with_style_and_box_metrics().is_root_element();
    if (is_root) {
        background_rect = absolute_border_box_rect();

        // Section 2.11.2: If the computed value of background-image on the root element is none and its background-color is transparent,
        // user agents must instead propagate the computed values of the background properties from that element’s first HTML BODY child element.
        auto& html_element = as<HTML::HTMLHtmlElement>(*layout_node_with_style_and_box_metrics().dom_node());
        if (html_element.should_use_body_background_properties()) {
            background_layers = document().background_layers();
            background_color = document().background_color();
        }
    } else {
        background_rect = absolute_padding_box_rect();
    }

    // HACK: If the Box has a border, use the bordered_rect to paint the background.
    //       This way if we have a border-radius there will be no gap between the filling and actual border.
    if (computed_values.border_top().width != 0 || computed_values.border_right().width != 0 || computed_values.border_bottom().width != 0 || computed_values.border_left().width != 0)
        background_rect = absolute_border_box_rect();

    m_resolved_background.layers.clear();
    if (background_layers)
        m_resolved_background = resolve_background_layers(*background_layers, *this, background_color, computed_values.background_color_clip(), background_rect, normalized_border_radii_data());

    if (is_root) {
        auto canvas_rect = navigable()->viewport_rect();
        if (auto overflow_rect = scrollable_overflow_rect(); overflow_rect.has_value())
            canvas_rect.unite(overflow_rect.value());
        m_resolved_background.background_rect.unite(canvas_rect);
        m_resolved_background.color_box.rect.unite(canvas_rect);
    }

    if (auto mask_image = computed_values.mask_image()) {
        mask_image->resolve_for_size(layout_node_with_style_and_box_metrics(), absolute_padding_box_rect().size());
    }

    // Filters
    auto resolve_css_filter = [&](CSS::Filter const& computed_filter) -> ResolvedCSSFilter {
        ResolvedCSSFilter result;
        for (auto const& filter_operation : computed_filter.filters()) {
            filter_operation.visit(
                [&](CSS::FilterOperation::Blur const& blur) {
                    auto resolved_radius = blur.resolved_radius();
                    result.operations.empend(ResolvedCSSFilter::Blur {
                        .radius = CSSPixels::nearest_value_for(resolved_radius),
                    });
                },
                [&](CSS::FilterOperation::DropShadow const& drop_shadow) {
                    auto to_css_px = [&](NonnullRefPtr<CSS::StyleValue const> const& length) {
                        return CSS::Length::from_style_value(length, {}).absolute_length_to_px();
                    };
                    auto color_context = CSS::ColorResolutionContext::for_layout_node_with_style(layout_node_with_style_and_box_metrics());
                    auto resolved_color = drop_shadow.color
                        ? drop_shadow.color->to_color(color_context).value_or(computed_values.color())
                        : computed_values.color();

                    result.operations.empend(ResolvedCSSFilter::DropShadow {
                        .offset_x = to_css_px(drop_shadow.offset_x),
                        .offset_y = to_css_px(drop_shadow.offset_y),
                        .radius = drop_shadow.radius ? to_css_px(*drop_shadow.radius) : CSSPixels(0),
                        .color = resolved_color,
                    });
                },
                [&](CSS::FilterOperation::Color const& color_operation) {
                    result.operations.empend(ResolvedCSSFilter::Color {
                        .operation = color_operation.operation,
                        .amount = color_operation.resolved_amount(),
                    });
                },
                [&](CSS::FilterOperation::HueRotate const& hue_rotate) {
                    result.operations.empend(ResolvedCSSFilter::HueRotate {
                        .angle_degrees = hue_rotate.angle_degrees(),
                    });
                },
                [&](CSS::URL const& css_url) {
                    auto& url_string = css_url.url();
                    if (url_string.is_empty() || !url_string.starts_with('#'))
                        return;
                    auto fragment_or_error = url_string.substring_from_byte_offset(1);
                    if (fragment_or_error.is_error())
                        return;
                    auto maybe_filter = document().get_element_by_id(fragment_or_error.value());
                    if (!maybe_filter)
                        return;
                    if (auto* filter_element = as_if<SVG::SVGFilterElement>(*maybe_filter)) {
                        auto& node = layout_node_with_style_and_box_metrics();
                        result.svg_filter = filter_element->gfx_filter(node);
                        // Compute bounds for triggering filter application.
                        // For empty elements (like <use> with no href), use the containing SVG's viewport.
                        auto bounds = absolute_border_box_rect();
                        if (bounds.is_empty()) {
                            if (auto const* svg_ancestor = first_ancestor_of_type<SVGSVGPaintable>())
                                result.svg_filter_bounds = svg_ancestor->absolute_rect();
                        }
                        if (!bounds.is_empty())
                            result.svg_filter_bounds = bounds;
                    }
                });
        }
        return result;
    };

    if (computed_values.filter().has_filters())
        set_filter(resolve_css_filter(computed_values.filter()));
    else
        set_filter({});

    if (computed_values.backdrop_filter().has_filters())
        set_backdrop_filter(resolve_css_filter(computed_values.backdrop_filter()));
    else
        set_backdrop_filter({});
}

RefPtr<ScrollFrame const> PaintableBox::nearest_scroll_frame() const
{
    if (is_fixed_position())
        return nullptr;
    auto const* paintable = this->containing_block();
    while (paintable) {
        if (paintable->own_scroll_frame())
            return paintable->own_scroll_frame();
        // Sticky elements need to find a scroll container even through fixed-position ancestors,
        // because they must reference a scrollport for their sticky offset computation.
        if (paintable->is_fixed_position() && !is_sticky_position())
            return nullptr;
        paintable = paintable->containing_block();
    }
    return nullptr;
}

PaintableBox const* PaintableBox::nearest_scrollable_ancestor() const
{
    auto const* paintable = this->containing_block();
    while (paintable) {
        if (paintable->could_be_scrolled_by_wheel_event())
            return paintable;
        if (paintable->is_fixed_position())
            return nullptr;
        paintable = paintable->containing_block();
    }
    return nullptr;
}

static PhysicalResizeAxes compute_physical_resize_axes(CSS::ComputedValues const& computed)
{
    // https://drafts.csswg.org/css-ui/#resize
    if (computed.resize() == CSS::Resize::None)
        return {};

    // 4.1. ... The resize property applies to elements that are scroll containers. UAs may also apply it,
    // regardless of the value of the overflow property, to:
    // - Replaced elements representing images or videos, such as img, video, picture, svg, object, or canvas.
    // - The <iframe> element.
    if (computed.display().is_inline_outside() && computed.display().is_flow_inside())
        return {};

    bool horizontal_writing_mode = computed.writing_mode() == CSS::WritingMode::HorizontalTb;

    return {
        .horizontal = computed.overflow_x() != CSS::Overflow::Visible
            && computed.overflow_x() != CSS::Overflow::Clip
            && (computed.resize() == CSS::Resize::Both
                || computed.resize() == CSS::Resize::Horizontal
                || (computed.resize() == CSS::Resize::Inline && horizontal_writing_mode)
                || (computed.resize() == CSS::Resize::Block && !horizontal_writing_mode)),
        .vertical = computed.overflow_y() != CSS::Overflow::Visible
            && computed.overflow_y() != CSS::Overflow::Clip
            && (computed.resize() == CSS::Resize::Both
                || computed.resize() == CSS::Resize::Vertical
                || (computed.resize() == CSS::Resize::Inline && !horizontal_writing_mode)
                || (computed.resize() == CSS::Resize::Block && horizontal_writing_mode))
    };
}

}
