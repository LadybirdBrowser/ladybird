/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/GenericShorthands.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Font/Font.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/FlexboxInspectorOverlay.h>
#include <LibWeb/Painting/GridInspectorOverlay.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/SVGPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/ShadowPainting.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/TableBordersPainting.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGFilterElement.h>

namespace Web::Painting {

static bool g_paint_viewport_scrollbars = true;

struct PaintableBox::CachedPaintData {
    bool has(PaintPhase phase) const
    {
        return m_present_phases[to_underlying(phase)];
    }

    ReadonlyBytes bytes_for(PaintPhase phase) const
    {
        auto const& span = m_phase_spans[to_underlying(phase)];
        return m_command_bytes.span().slice(span.offset, span.size);
    }

    void set(PaintPhase phase, ReadonlyBytes command_bytes)
    {
        auto const phase_index = to_underlying(phase);
        if (m_present_phases[phase_index]) {
            replace(phase, command_bytes);
            return;
        }

        m_phase_spans[phase_index] = append_to(m_command_bytes, command_bytes);
        m_present_phases[phase_index] = true;
    }

    template<typename Callback>
    void for_each_present_phase(Callback callback) const
    {
        for (size_t phase_index = 0; phase_index < paint_phase_count; ++phase_index) {
            if (!m_present_phases[phase_index])
                continue;
            auto phase = static_cast<PaintPhase>(phase_index);
            callback(phase, bytes_for(phase));
        }
    }

private:
    struct Span {
        u32 offset { 0 };
        u32 size { 0 };
    };

    static Span append_to(ByteBuffer& command_buffer, ReadonlyBytes command_bytes)
    {
        auto const offset = command_buffer.size();
        command_buffer.append(command_bytes);
        return { static_cast<u32>(offset), static_cast<u32>(command_bytes.size()) };
    }

    void replace(PaintPhase phase, ReadonlyBytes replacement_bytes)
    {
        ByteBuffer command_buffer;
        Array<Span, paint_phase_count> phase_spans {};

        for (size_t phase_index = 0; phase_index < paint_phase_count; ++phase_index) {
            if (!m_present_phases[phase_index])
                continue;
            auto present_phase = static_cast<PaintPhase>(phase_index);
            auto command_bytes = present_phase == phase ? replacement_bytes : bytes_for(present_phase);
            phase_spans[phase_index] = append_to(command_buffer, command_bytes);
        }

        m_command_bytes = move(command_buffer);
        m_phase_spans = phase_spans;
    }

    ByteBuffer m_command_bytes;
    Array<bool, paint_phase_count> m_present_phases {};
    Array<Span, paint_phase_count> m_phase_spans {};
};

static bool content_size_change_affects_container_queries(PaintableBox const& paintable_box, CSSPixelSize old_size, CSSPixelSize new_size)
{
    auto const& container_type = paintable_box.computed_values().container_type();
    if (container_type.is_size_container)
        return old_size != new_size;

    if (!container_type.is_inline_size_container)
        return false;

    if (paintable_box.computed_values().writing_mode() == CSS::WritingMode::HorizontalTb)
        return old_size.width() != new_size.width();

    return old_size.height() != new_size.height();
}

static void invalidate_descendant_styles_for_container_query_size_change(PaintableBox& paintable_box, CSSPixelSize old_size, CSSPixelSize new_size)
{
    if (!content_size_change_affects_container_queries(paintable_box, old_size, new_size))
        return;

    if (auto* element = as_if<DOM::Element>(paintable_box.dom_node().ptr())) {
        element->for_each_shadow_including_descendant([](DOM::Node& node) {
            if (auto* descendant_element = as_if<DOM::Element>(node); descendant_element && descendant_element->style_depends_on_size_container_query())
                descendant_element->set_needs_style_update(true);
            return TraversalDecision::Continue;
        });
    }
}

void set_paint_viewport_scrollbars(bool const enabled)
{
    g_paint_viewport_scrollbars = enabled;
}

bool should_paint_viewport_scrollbars()
{
    return g_paint_viewport_scrollbars;
}

static Gfx::FloatPoint css_point_to_device_point(CSSPixelPoint point, double device_pixels_per_css_pixel)
{
    auto scale = static_cast<float>(device_pixels_per_css_pixel);
    return { point.x().to_float() * scale, point.y().to_float() * scale };
}

static Gfx::FloatSize css_size_to_device_size(CSSPixelSize size, double device_pixels_per_css_pixel)
{
    auto scale = static_cast<float>(device_pixels_per_css_pixel);
    return { size.width().to_float() * scale, size.height().to_float() * scale };
}

static Gfx::FloatRect css_rect_to_device_rect(CSSPixelRect rect, double device_pixels_per_css_pixel)
{
    return { css_point_to_device_point(rect.location(), device_pixels_per_css_pixel), css_size_to_device_size(rect.size(), device_pixels_per_css_pixel) };
}

static Optional<float> css_inset_to_device_inset(Optional<CSSPixels> inset, double device_pixels_per_css_pixel)
{
    if (!inset.has_value())
        return {};
    return inset->to_float() * static_cast<float>(device_pixels_per_css_pixel);
}

static Optional<CompositorScrollNodeKind> scroll_node_kind_for(PaintableBox const& paintable_box)
{
    if (paintable_box.is_viewport_paintable())
        return CompositorScrollNodeKind::Viewport;
    if (paintable_box.layout_node().generated_for_pseudo_element().has_value())
        return CompositorScrollNodeKind::PseudoElement;
    if (paintable_box.dom_node() && is<DOM::Element>(*paintable_box.dom_node()))
        return CompositorScrollNodeKind::Element;
    return {};
}

static UniqueNodeID scrollable_node_id_for(PaintableBox const& paintable_box)
{
    if (paintable_box.is_viewport_paintable())
        return paintable_box.document().unique_id();
    if (paintable_box.layout_node().generated_for_pseudo_element().has_value())
        return paintable_box.layout_node().pseudo_element_generator()->unique_id();
    return paintable_box.dom_node()->unique_id();
}

static u8 pseudo_element_type_for(PaintableBox const& paintable_box)
{
    auto pseudo_element = paintable_box.layout_node().generated_for_pseudo_element();
    if (!pseudo_element.has_value())
        return 0;
    return static_cast<u8>(to_underlying(*pseudo_element));
}

static bool is_nested_navigable_container(PaintableBox const& paintable_box)
{
    auto node = paintable_box.dom_node();
    return node && node->is_navigable_container() && as<HTML::NavigableContainer const>(*node).content_navigable();
}

static CSSPixelPoint maximum_scroll_offset_for(PaintableBox const& paintable_box)
{
    CSSPixelPoint max_scroll_offset;
    auto scrollable_overflow_rect = paintable_box.scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return max_scroll_offset;

    auto scrollport_rect = paintable_box.absolute_padding_box_rect();

    max_scroll_offset.set_x(max(CSSPixels(0), scrollable_overflow_rect->width() - scrollport_rect.width()));
    max_scroll_offset.set_y(max(CSSPixels(0), scrollable_overflow_rect->height() - scrollport_rect.height()));
    return max_scroll_offset;
}

static void record_scroll_node(PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    auto scroll_node_kind = scroll_node_kind_for(paintable_box);
    if (!scroll_node_kind.has_value())
        return;

    auto parent_scroll_frame_index = ScrollFrameIndex {};
    if (auto scrollable_ancestor = paintable_box.nearest_scrollable_ancestor())
        parent_scroll_frame_index = scrollable_ancestor->own_scroll_frame_index();

    auto scrollport_rect = paintable_box.is_viewport_paintable()
        ? Gfx::IntRect { {}, context.device_viewport_rect().size().to_type<int>() }
        : context.rounded_device_rect(paintable_box.absolute_padding_box_rect()).to_type<int>();

    auto& recorder = context.display_list_recorder();
    recorder.compositor_scroll_node({
        .document_id = paintable_box.document().unique_id(),
        .scrollable_node_id = scrollable_node_id_for(paintable_box),
        .scroll_frame_index = paintable_box.own_scroll_frame_index(),
        .parent_scroll_frame_index = parent_scroll_frame_index,
        .scrollport_rect = scrollport_rect,
        .max_scroll_offset = css_point_to_device_point(maximum_scroll_offset_for(paintable_box), context.device_pixels_per_css_pixel()),
        .scroll_node_kind = *scroll_node_kind,
        .pseudo_element_type = pseudo_element_type_for(paintable_box),
        .is_viewport = paintable_box.is_viewport_paintable(),
        .can_be_wheel_scrolled_horizontally = paintable_box.could_be_scrolled_by_wheel_event(PaintableBox::ScrollDirection::Horizontal),
        .can_be_wheel_scrolled_vertically = paintable_box.could_be_scrolled_by_wheel_event(PaintableBox::ScrollDirection::Vertical),
    });
}

static void record_main_thread_wheel_event_region(PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    auto rect = css_rect_to_device_rect(paintable_box.absolute_united_border_box_rect(), context.device_pixels_per_css_pixel());
    if (rect.is_empty())
        return;

    context.display_list_recorder().compositor_main_thread_wheel_event_region({
        .rect = rect,
    });
}

static Optional<ScrollFrameIndex> wheel_hit_test_target_scroll_frame_index_for(PaintableBox const& paintable_box)
{
    if (paintable_box.own_scroll_frame_index().value() && paintable_box.could_be_scrolled_by_wheel_event())
        return paintable_box.own_scroll_frame_index();
    if (auto scrollable_ancestor = paintable_box.nearest_scrollable_ancestor())
        return scrollable_ancestor->own_scroll_frame_index();
    if (auto viewport_paintable = paintable_box.document().paintable(); viewport_paintable && viewport_paintable->could_be_scrolled_by_wheel_event())
        return viewport_paintable->own_scroll_frame_index();
    return {};
}

static void record_wheel_hit_test_target(PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    if (!paintable_box.is_visible() || !paintable_box.visible_for_hit_testing())
        return;

    auto rect = css_rect_to_device_rect(paintable_box.absolute_border_box_rect(), context.device_pixels_per_css_pixel());
    if (rect.is_empty())
        return;

    auto target_scroll_frame_index = wheel_hit_test_target_scroll_frame_index_for(paintable_box).value_or({});
    auto corner_radii = paintable_box.border_radii_data().as_corners(context.device_pixel_converter());
    if (corner_radii.has_any_radius()) {
        context.display_list_recorder().compositor_wheel_hit_test_target_with_corner_radii({
            .document_id = paintable_box.document().unique_id(),
            .target_scroll_frame_index = target_scroll_frame_index,
            .rect = rect,
            .corner_radii = corner_radii,
        });
        return;
    }

    context.display_list_recorder().compositor_wheel_hit_test_target({
        .document_id = paintable_box.document().unique_id(),
        .target_scroll_frame_index = target_scroll_frame_index,
        .rect = rect,
    });
}

static void record_viewport_scrollbar_state(PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    if (!paintable_box.is_viewport_paintable())
        return;
    if (!paintable_box.document().page().async_scrolling_enabled())
        return;
    if (!should_paint_viewport_scrollbars())
        return;
    if (paintable_box.computed_values().scrollbar_width() == CSS::ScrollbarWidth::None)
        return;

    auto scrollbar_colors = paintable_box.computed_values().scrollbar_color();
    auto const& metrics = context.chrome_metrics();

    for (auto direction : { PaintableBox::ScrollDirection::Vertical, PaintableBox::ScrollDirection::Horizontal }) {
        auto scrollbar_data = paintable_box.compute_scrollbar_data(direction, metrics, nullptr, PaintableBox::ScrollbarSizing::Regular);
        if (!scrollbar_data.has_value())
            continue;
        auto expanded_scrollbar_data = paintable_box.compute_scrollbar_data(direction, metrics, nullptr, PaintableBox::ScrollbarSizing::Enlarged);
        VERIFY(expanded_scrollbar_data.has_value());

        auto gutter_rect = context.rounded_device_rect(scrollbar_data->gutter_rect).to_type<int>();
        auto max_scroll_offset = css_point_to_device_point(maximum_scroll_offset_for(paintable_box), context.device_pixels_per_css_pixel());
        auto orientation = direction == PaintableBox::ScrollDirection::Horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
        auto thumb_color = scrollbar_colors.thumb_color;
        if (gutter_rect.is_empty() && thumb_color == CSS::InitialValues::scrollbar_color().thumb_color)
            thumb_color = thumb_color.with_alpha(128);

        context.display_list_recorder().compositor_viewport_scrollbar({
            .document_id = paintable_box.document().unique_id(),
            .scroll_frame_index = paintable_box.own_scroll_frame_index(),
            .gutter_rect = gutter_rect,
            .thumb_rect = context.rounded_device_rect(scrollbar_data->thumb_rect).to_type<int>(),
            .expanded_gutter_rect = context.rounded_device_rect(expanded_scrollbar_data->gutter_rect).to_type<int>(),
            .expanded_thumb_rect = context.rounded_device_rect(expanded_scrollbar_data->thumb_rect).to_type<int>(),
            .scroll_size = scrollbar_data->thumb_travel_to_scroll_ratio.to_double(),
            .expanded_scroll_size = expanded_scrollbar_data->thumb_travel_to_scroll_ratio.to_double(),
            .max_scroll_offset = max_scroll_offset.primary_offset_for_orientation(orientation),
            .thumb_color = thumb_color,
            .track_color = scrollbar_colors.track_color,
            .vertical = direction == PaintableBox::ScrollDirection::Vertical,
        });
    }
}

static void record_blocking_wheel_event_region(PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    if (context.has_blocking_wheel_event_region_covering_viewport())
        return;
    if (!paintable_box.is_visible() || !paintable_box.visible_for_hit_testing())
        return;
    auto node = paintable_box.dom_node();
    if (!node || !node->inside_blocking_wheel_event_handler())
        return;

    auto rect = css_rect_to_device_rect(paintable_box.absolute_united_border_box_rect(), context.device_pixels_per_css_pixel());
    if (rect.is_empty())
        return;

    context.set_has_blocking_wheel_event_listeners(true);
    context.display_list_recorder().compositor_blocking_wheel_event_region({
        .rect = rect,
    });
}

ResolvedCSSFilter resolve_css_filter(CSS::Filter const& computed_filter, PaintableBox const& paintable_box)
{
    auto const& computed_values = paintable_box.computed_values();
    auto const& layout_node = paintable_box.layout_node_with_style_and_box_metrics();

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
                auto color_context = CSS::ColorResolutionContext::for_layout_node_with_style(layout_node);
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
                auto maybe_filter = paintable_box.document().get_element_by_id(fragment_or_error.value());
                if (!maybe_filter)
                    return;
                if (auto* filter_element = as_if<SVG::SVGFilterElement>(*maybe_filter)) {
                    result.svg_filter = filter_element->gfx_filter(layout_node);
                    auto bounds = paintable_box.absolute_border_box_rect();
                    if (bounds.is_empty()) {
                        if (auto svg_ancestor = paintable_box.first_ancestor_of_type<SVGSVGPaintable>())
                            result.svg_filter_bounds = svg_ancestor->absolute_rect();
                    }
                    if (!bounds.is_empty())
                        result.svg_filter_bounds = bounds;
                }
            });
    }
    return result;
}

NonnullRefPtr<PaintableBox> PaintableBox::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new PaintableBox(layout_box));
}

NonnullRefPtr<PaintableBox> PaintableBox::create(Layout::InlineNode const& layout_box)
{
    return adopt_ref(*new PaintableBox(layout_box));
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
    if (has_layout_node())
        invalidate_paint_cache();
}

void PaintableBox::acquire_cache_references_for_cached_commands(ReadonlyBytes command_bytes) const
{
    auto& resource_storage = navigable()->display_list_resource_storage();
    auto referenced_resources = resource_storage.collect_referenced_resources(command_bytes);
    if (referenced_resources.is_empty())
        return;
    resource_storage.acquire_cache_references(referenced_resources);
}

void PaintableBox::release_cache_references_for_cached_commands(ReadonlyBytes command_bytes) const
{
    auto& resource_storage = navigable()->display_list_resource_storage();
    auto referenced_resources = resource_storage.collect_referenced_resources(command_bytes);
    if (referenced_resources.is_empty())
        return;
    resource_storage.release_cache_references(referenced_resources);
}

bool PaintableBox::has_cached_commands(PaintPhase phase) const
{
    return m_cached_paint_data && m_cached_paint_data->has(phase);
}

ReadonlyBytes PaintableBox::cached_commands(PaintPhase phase) const
{
    return m_cached_paint_data->bytes_for(phase);
}

void PaintableBox::invalidate_paint_cache() const
{
    if (!m_cached_paint_data)
        return;

    m_cached_paint_data->for_each_present_phase([&](PaintPhase, ReadonlyBytes command_bytes) {
        release_cache_references_for_cached_commands(command_bytes);
    });
    m_cached_paint_data = nullptr;
}

void PaintableBox::set_cached_commands(PaintPhase phase, ByteBuffer const& commands) const
{
    if (!m_cached_paint_data)
        m_cached_paint_data = make<CachedPaintData>();

    if (m_cached_paint_data->has(phase))
        release_cache_references_for_cached_commands(m_cached_paint_data->bytes_for(phase));

    auto command_bytes = commands.span();
    acquire_cache_references_for_cached_commands(command_bytes);
    m_cached_paint_data->set(phase, command_bytes);
}

void PaintableBox::reset_for_relayout()
{
    if (parent())
        remove();
    while (first_child())
        first_child()->remove();

    m_containing_block = {};

    m_offset = {};
    m_content_size = {};

    m_box_model = {};

    m_overflow_data.clear();
    m_override_borders_data.clear();
    m_table_cell_coordinates.clear();
    m_containing_line_box_data.clear();
    m_sticky_insets = nullptr;

    m_absolute_rect.clear();
    m_absolute_padding_box_rect.clear();
    m_absolute_border_box_rect.clear();

    m_enclosing_scroll_frame_index = {};
    m_own_scroll_frame_index = {};
    m_accumulated_visual_context_index = VISUAL_VIEWPORT_NODE_INDEX;
    m_accumulated_visual_context_for_descendants_index = VISUAL_VIEWPORT_NODE_INDEX;
    m_fixed_background_visual_context = {};

    m_used_values_for_grid_template_columns = nullptr;
    m_used_values_for_grid_template_rows = nullptr;
    m_grid_layout_data = nullptr;
    m_flex_layout_data = nullptr;

    invalidate_paint_cache();

    invalidate_stacking_context();
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

Optional<CSSPixelRect> PaintableBox::absolute_containing_line_box_rect() const
{
    if (!m_containing_line_box_data.has_value())
        return {};

    auto rect = m_containing_line_box_data->rect;
    if (auto containing_block = this->containing_block())
        rect.translate_by(containing_block->absolute_position());
    return rect;
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

    set_needs_repaint(InvalidateDisplayList::No);
    return ScrollHandled::Yes;
}

PaintableBox::ScrollHandled PaintableBox::scroll_by(double delta_x, double delta_y)
{
    return set_scroll_offset(scroll_offset().translated(CSSPixels::nearest_value_for(delta_x), CSSPixels::nearest_value_for(delta_y)));
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
    auto old_size = m_content_size;
    m_content_size = size;
    if (auto layout_box = as_if<Layout::Box>(layout_node()))
        layout_box->did_set_content_size();
    invalidate_descendant_styles_for_container_query_size_change(*this, old_size, size);
}

void PaintableBox::set_fragmentation_state(FragmentationState fragmentation_state)
{
    switch (fragmentation_state) {
    case FragmentationState::Unfragmented:
        break;
    case FragmentationState::HorizontalStart:
        m_fragment_right_edge_away = true;
        break;
    case FragmentationState::HorizontalMiddle:
        m_fragment_left_edge_away = true;
        m_fragment_right_edge_away = true;
        break;
    case FragmentationState::HorizontalEnd:
        m_fragment_left_edge_away = true;
        break;
    case FragmentationState::VerticalStart:
        m_fragment_bottom_edge_away = true;
        break;
    case FragmentationState::VerticalMiddle:
        m_fragment_top_edge_away = true;
        m_fragment_bottom_edge_away = true;
        break;
    case FragmentationState::VerticalEnd:
        m_fragment_top_edge_away = true;
        break;
    }
}

CSSPixelPoint PaintableBox::offset() const
{
    return m_offset;
}

CSSPixelRect PaintableBox::compute_absolute_rect() const
{
    CSSPixelRect rect { offset(), content_size() };
    for (auto block = containing_block(); block; block = block->containing_block())
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
        auto border_top = m_fragment_top_edge_away ? 0 : box_model().border.top;
        auto border_bottom = m_fragment_bottom_edge_away ? 0 : box_model().border.bottom;
        auto border_left = m_fragment_left_edge_away ? 0 : box_model().border.left;
        auto border_right = m_fragment_right_edge_away ? 0 : box_model().border.right;
        if (use_collapsing_borders_model) {
            border_top = round(border_top / 2);
            border_bottom = round(border_bottom / 2);
            border_left = round(border_left / 2);
            border_right = round(border_right / 2);
        }
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
    auto const& overflow_clip_margin = computed_values().overflow_clip_margin();
    auto resolve_box_edge = [&](CSS::OverflowClipMarginSide const& side) -> CSSPixelRect {
        auto box = side.visual_box;
        if (!box.has_value()) {
            if (layout_node().is_replaced_box())
                box = CSS::BackgroundBox::ContentBox;
            else
                box = CSS::BackgroundBox::PaddingBox;
        }
        switch (*box) {
        case CSS::BackgroundBox::ContentBox:
            return absolute_rect();
        case CSS::BackgroundBox::BorderBox:
            return absolute_border_box_rect();
        case CSS::BackgroundBox::PaddingBox:
        default:
            return absolute_padding_box_rect();
        }
    };

    auto overflow_clip_edge = resolve_box_edge(overflow_clip_margin.top);

    // '<length [0,∞]>'
    //     The specified offset dictates how much the overflow clip edge is expanded from the specified box edge
    //     Negative values are invalid. Defaults to zero if omitted.
    overflow_clip_edge.inflate(
        overflow_clip_margin.top.offset.absolute_length_to_px(),
        overflow_clip_margin.right.offset.absolute_length_to_px(),
        overflow_clip_margin.bottom.offset.absolute_length_to_px(),
        overflow_clip_margin.left.offset.absolute_length_to_px());
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
            auto const& paintable_box = static_cast<PaintableBox const&>(*paintable);
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
        return clip.to_rect().resolved(border_box);
    }
    return {};
}

RefPtr<Scrollbar> PaintableBox::scrollbar(ScrollDirection direction) const
{
    return direction == ScrollDirection::Horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
}

NonnullRefPtr<Scrollbar> PaintableBox::ensure_scrollbar(ScrollDirection direction)
{
    auto& slot = direction == ScrollDirection::Horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
    if (!slot)
        slot = Scrollbar::create(const_cast<PaintableBox&>(*this), direction);
    return *slot;
}

static CSS::Overflow overflow_value_applied_to_viewport_for_wheel_scrolling(DOM::Document const& document, PaintableBox::ScrollDirection direction)
{
    auto overflow_for_direction = [direction](CSS::ComputedProperties const& computed_properties) {
        return direction == PaintableBox::ScrollDirection::Horizontal
            ? computed_properties.overflow_x()
            : computed_properties.overflow_y();
    };

    auto* root_element = document.document_element();
    if (!root_element || !root_element->computed_properties())
        return CSS::Overflow::Auto;

    auto* overflow_origin_element = root_element;
    if (root_element->is_html_html_element() && root_element->computed_properties()->contain().is_empty()) {
        auto root_overflow_x = root_element->computed_properties()->overflow_x();
        auto root_overflow_y = root_element->computed_properties()->overflow_y();
        if (root_overflow_x == CSS::Overflow::Visible && root_overflow_y == CSS::Overflow::Visible) {
            auto* body_element = root_element->first_child_of_type<HTML::HTMLBodyElement>();
            if (body_element && body_element->computed_properties() && body_element->computed_properties()->contain().is_empty())
                overflow_origin_element = body_element;
        }
    }

    auto overflow = overflow_for_direction(*overflow_origin_element->computed_properties());
    if (overflow == CSS::Overflow::Visible)
        return CSS::Overflow::Auto;
    if (overflow == CSS::Overflow::Clip)
        return CSS::Overflow::Hidden;
    return overflow;
}

bool PaintableBox::could_be_scrolled_by_wheel_event(ScrollDirection direction) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    Gfx::Orientation orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto overflow = is_horizontal ? computed_values().overflow_x() : computed_values().overflow_y();
    if (is_viewport_paintable())
        overflow = overflow_value_applied_to_viewport_for_wheel_scrolling(document(), direction);

    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return false;

    CSSPixels scrollable_overflow_size = scrollable_overflow_rect->primary_size_for_orientation(orientation);
    CSSPixels scrollport_size = absolute_padding_box_rect().primary_size_for_orientation(orientation);

    if (overflow == CSS::Overflow::Auto || overflow == CSS::Overflow::Scroll)
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

Optional<PaintableBox::ScrollbarData> PaintableBox::compute_scrollbar_data(ScrollDirection direction, ChromeMetrics const& metrics, ScrollStateSnapshot const* scroll_state_snapshot, ScrollbarSizing scrollbar_sizing) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto overflow = is_horizontal ? computed_values().overflow_x() : computed_values().overflow_y();

    if (overflow != CSS::Overflow::Scroll && !could_be_scrolled_by_wheel_event(direction))
        return {};

    if (!m_own_scroll_frame_index.value())
        return {};

    CSSPixelRect scrollable_overflow_rect = this->scrollable_overflow_rect().value();
    CSSPixels scrollable_overflow_length = scrollable_overflow_rect.primary_size_for_orientation(orientation);
    if (scrollable_overflow_length == 0)
        return {};

    auto const& scrollbar = is_horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
    bool with_gutter = [&] {
        switch (scrollbar_sizing) {
        case ScrollbarSizing::Current:
            return scrollbar && scrollbar->is_enlarged();
        case ScrollbarSizing::Regular:
            return false;
        case ScrollbarSizing::Enlarged:
            return true;
        }
        VERIFY_NOT_REACHED();
    }();
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
        auto own_offset = scroll_state_snapshot->device_offset_for_index(m_own_scroll_frame_index);
        auto device_scroll_offset = is_horizontal ? -own_offset.x() : -own_offset.y();
        auto device_pixels_per_css_pixel = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
        CSSPixels thumb_offset = CSSPixels::nearest_value_for(device_scroll_offset / device_pixels_per_css_pixel) * scrollbar_data.thumb_travel_to_scroll_ratio;
        scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_offset);
    }

    return scrollbar_data;
}

void PaintableBox::record_async_scrolling_metadata(DisplayListRecordingContext& context) const
{
    if (!context.is_recording_async_scrolling_metadata())
        return;

    auto device_pixels_per_css_pixel = context.device_pixels_per_css_pixel();
    auto& recorder = context.display_list_recorder();

    record_wheel_hit_test_target(*this, context);

    record_blocking_wheel_event_region(*this, context);

    if (is_nested_navigable_container(*this)) {
        record_main_thread_wheel_event_region(*this, context);
    } else if (own_scroll_frame_index().value() && could_be_scrolled_by_wheel_event()) {
        record_scroll_node(*this, context);
    }
    record_viewport_scrollbar_state(*this, context);

    auto const& scroll_state = context.async_scrolling_scroll_state();
    auto sticky_frame_index = enclosing_scroll_frame_index();
    if (is_sticky_position() && sticky_frame_index.value()) {
        auto const& frame = scroll_state.frame_at(sticky_frame_index);
        if (frame.is_sticky() && frame.has_sticky_constraints()) {
            auto const& constraints = frame.sticky_constraints();
            auto const& insets = constraints.insets;
            recorder.compositor_sticky_area({
                .document_id = context.async_scrolling_document_id(),
                .scroll_frame_index = sticky_frame_index,
                .parent_scroll_frame_index = frame.parent_index(),
                .nearest_scrolling_ancestor_index = scroll_state.nearest_scrolling_ancestor(sticky_frame_index),
                .position_relative_to_scroll_ancestor = css_point_to_device_point(constraints.position_relative_to_scroll_ancestor, device_pixels_per_css_pixel),
                .border_box_size = css_size_to_device_size(constraints.border_box_size, device_pixels_per_css_pixel),
                .scrollport_size = css_size_to_device_size(constraints.scrollport_size, device_pixels_per_css_pixel),
                .containing_block_region = css_rect_to_device_rect(constraints.containing_block_region, device_pixels_per_css_pixel),
                .needs_parent_offset_adjustment = constraints.needs_parent_offset_adjustment,
                .inset_top = css_inset_to_device_inset(insets.top, device_pixels_per_css_pixel),
                .inset_right = css_inset_to_device_inset(insets.right, device_pixels_per_css_pixel),
                .inset_bottom = css_inset_to_device_inset(insets.bottom, device_pixels_per_css_pixel),
                .inset_left = css_inset_to_device_inset(insets.left, device_pixels_per_css_pixel),
            });
        }
    }
}

void PaintableBox::record_hit_test_items(DisplayListRecordingContext& context, PaintPhase phase) const
{
    auto* hit_test_display_list = context.hit_test_display_list();
    if (!hit_test_display_list)
        return;

    auto const is_visible = computed_values().visibility() == CSS::Visibility::Visible;
    if (!is_visible || !visible_for_hit_testing())
        return;

    if (phase == PaintPhase::Background) {
        Paintable* target = const_cast<PaintableBox*>(this);
        if (layout_node().is_anonymous()
            && !layout_node().is_generated_for_pseudo_element()
            && !layout_node().is_list_item_marker_box()) {
            auto continuation_node = layout_node_with_style_and_box_metrics().continuation_of_node();
            if (!continuation_node)
                return;
            while (continuation_node->continuation_of_node())
                continuation_node = continuation_node->continuation_of_node();
            auto& continuation_paintable = *continuation_node->first_paintable();
            if (!continuation_paintable.visible_for_hit_testing())
                return;
            target = &continuation_paintable;
        }

        hit_test_display_list->append_box(*this, *target, absolute_border_box_rect(), accumulated_visual_context_index(), border_radii_data());
        return;
    }

    if (phase != PaintPhase::Overlay)
        return;

    auto& box = const_cast<PaintableBox&>(*this);
    if (has_resizer())
        hit_test_display_list->append_chrome_widget(*this, box.ensure_resize_handle(), accumulated_visual_context_index());

    if (could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal))
        hit_test_display_list->append_chrome_widget(*this, box.ensure_scrollbar(ScrollDirection::Horizontal), accumulated_visual_context_index());
    if (could_be_scrolled_by_wheel_event(ScrollDirection::Vertical))
        hit_test_display_list->append_chrome_widget(*this, box.ensure_scrollbar(ScrollDirection::Vertical), accumulated_visual_context_index());
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

        if (((g_paint_viewport_scrollbars && !document().page().async_scrolling_enabled()) || !is_viewport_paintable())
            && computed_values().scrollbar_width() != CSS::ScrollbarWidth::None) {
            auto scrollbar_colors = computed_values().scrollbar_color();

            for (auto direction : { ScrollDirection::Vertical, ScrollDirection::Horizontal }) {
                auto scrollbar_data = compute_scrollbar_data(direction, metrics);
                if (!scrollbar_data.has_value())
                    continue;
                auto gutter_rect = context.rounded_device_rect(scrollbar_data->gutter_rect).to_type<int>();
                auto thumb_color = scrollbar_colors.thumb_color;
                if (gutter_rect.is_empty() && thumb_color == CSS::InitialValues::scrollbar_color().thumb_color)
                    thumb_color = thumb_color.with_alpha(128);
                context.display_list_recorder().paint_scrollbar(
                    m_own_scroll_frame_index,
                    gutter_rect,
                    context.rounded_device_rect(scrollbar_data->thumb_rect).to_type<int>(),
                    scrollbar_data->thumb_travel_to_scroll_ratio.to_double(),
                    thumb_color,
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

        paint_middle_button_scroll_indicator(context);
    }
}

void PaintableBox::paint_middle_button_scroll_indicator(DisplayListRecordingContext& context) const
{
    static constexpr Gfx::Color CIRCLE_COLOR = Gfx::Color { Gfx::Color::White }.with_alpha(220);
    static constexpr Gfx::Color ARROW_COLOR = Gfx::Color::DarkGray;

    static constexpr auto RADIUS = 16;
    static constexpr auto ARROW_SIZE = 6;
    static constexpr auto ARROW_OFFSET = 8;

    if (!is_viewport_paintable())
        return;

    auto navigable = document().navigable();
    if (!navigable)
        return;

    auto handler = navigable->event_handler().middle_button_scroll_handler();
    if (!handler.has_value())
        return;

    auto& recorder = context.display_list_recorder();
    auto device_origin = context.rounded_device_point(handler->origin()).to_type<int>();

    Gfx::IntRect circle { device_origin.x() - RADIUS, device_origin.y() - RADIUS, RADIUS * 2, RADIUS * 2 };
    recorder.fill_ellipse(circle, CIRCLE_COLOR);
    recorder.draw_ellipse(circle, ARROW_COLOR, 1);

    auto paint_arrow = [&](Gfx::FloatPoint p1, Gfx::FloatPoint p2, Gfx::FloatPoint p3) {
        Gfx::Path path;
        path.move_to(p1);
        path.line_to(p2);
        path.line_to(p3);
        path.close();

        recorder.fill_path({ .path = move(path), .paint_style_or_color = ARROW_COLOR });
    };

    auto x = static_cast<float>(device_origin.x());
    auto y = static_cast<float>(device_origin.y());

    // FIXME: We could paint a subset of these arrows depending on which direction the container may be scrolled.
    paint_arrow({ x, y - ARROW_OFFSET - ARROW_SIZE }, { x - ARROW_SIZE, y - ARROW_OFFSET }, { x + ARROW_SIZE, y - ARROW_OFFSET });
    paint_arrow({ x, y + ARROW_OFFSET + ARROW_SIZE }, { x - ARROW_SIZE, y + ARROW_OFFSET }, { x + ARROW_SIZE, y + ARROW_OFFSET });
    paint_arrow({ x - ARROW_OFFSET - ARROW_SIZE, y }, { x - ARROW_OFFSET, y - ARROW_SIZE }, { x - ARROW_OFFSET, y + ARROW_SIZE });
    paint_arrow({ x + ARROW_OFFSET + ARROW_SIZE, y }, { x + ARROW_OFFSET, y - ARROW_SIZE }, { x + ARROW_OFFSET, y + ARROW_SIZE });
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

void PaintableBox::paint_grid_inspector_overlay(DisplayListRecordingContext& context, GridInspectorOverlayOptions const& options) const
{
    if (!m_grid_layout_data)
        return;

    paint_with_inspector_overlay_context(context, [&] {
        auto content_rect = absolute_united_content_rect();
        auto const origin = content_rect.location();
        auto const viewport_rect = document().viewport_rect();
        auto const& color = options.color;
        auto font = Platform::FontPlugin::the().default_font(10);
        auto label_font = font->with_size(font->point_size() * context.device_pixels_per_css_pixel());
        auto label_height = CSSPixels::nearest_value_for(font->pixel_size()) + 4;
        auto label_padding = CSSPixels(4);

        auto paint_rect = [&](CSSPixelRect const& rect, Gfx::Color rect_color) {
            context.display_list_recorder().fill_rect(context.enclosing_device_rect(rect).to_type<int>(), rect_color);
        };

        auto paint_label = [&](CSSPixelPoint top_left, Utf16String const& text) {
            auto label_width = CSSPixels::nearest_value_for(font->width(text)) + label_padding * 2;
            auto label_rect = CSSPixelRect { top_left.x(), top_left.y(), label_width, label_height };
            auto label_device_rect = context.enclosing_device_rect(label_rect).to_type<int>();
            auto label_background = color.with_alpha(235);
            context.display_list_recorder().fill_rect(label_device_rect, label_background);
            context.display_list_recorder().draw_rect(label_device_rect, color.with_alpha(255));
            context.display_list_recorder().draw_text(label_device_rect, text, label_font, Gfx::TextAlignment::Center, label_background.suggested_foreground_color());
        };

        auto paint_centered_label = [&](CSSPixelRect const& rect, Utf16String const& text) {
            auto label_width = CSSPixels::nearest_value_for(font->width(text)) + label_padding * 2;
            paint_label({ rect.center().x() - label_width / 2, rect.center().y() - label_height / 2 }, text);
        };

        auto line_start_for_number = [](Layout::GridLayoutDimension const& dimension, u32 number) -> Optional<CSSPixels> {
            for (auto const& line : dimension.lines) {
                if (line.number == number)
                    return line.start;
            }
            return {};
        };

        auto line_number_label = [](Layout::GridLayoutLine const& line) {
            if (line.negative_number < 0)
                return MUST(String::formatted("{} / {}", line.number, line.negative_number));
            return MUST(String::formatted("{}", line.number));
        };

        auto track_size_label = [](Layout::GridLayoutTrack const& track) {
            return MUST(String::formatted("{:.2f}px", max(0.0, track.breadth.to_double())));
        };

        auto line_color = color.with_alpha(220);
        auto gap_color = color.with_alpha(45);
        auto line_thickness = CSSPixels(1);

        for (auto const& fragment : m_grid_layout_data->fragments) {
            for (auto const& column_line : fragment.columns.lines) {
                auto x = origin.x() + column_line.start;
                auto line_top = options.show_infinite_lines ? viewport_rect.y() : content_rect.y();
                auto line_height = options.show_infinite_lines ? viewport_rect.height() : content_rect.height();

                if (column_line.breadth > 0)
                    paint_rect({ x, line_top, column_line.breadth, line_height }, gap_color);

                paint_rect({ x, line_top, line_thickness, line_height }, line_color);

                if (options.show_line_numbers)
                    paint_label({ x + 2, line_top + 2 }, Utf16String::from_utf8(line_number_label(column_line)));
            }

            for (auto const& row_line : fragment.rows.lines) {
                auto y = origin.y() + row_line.start;
                auto line_left = options.show_infinite_lines ? viewport_rect.x() : content_rect.x();
                auto line_width = options.show_infinite_lines ? viewport_rect.width() : content_rect.width();

                if (row_line.breadth > 0)
                    paint_rect({ line_left, y, line_width, row_line.breadth }, gap_color);

                paint_rect({ line_left, y, line_width, line_thickness }, line_color);

                if (options.show_line_numbers)
                    paint_label({ line_left + 2, y + 2 }, Utf16String::from_utf8(line_number_label(row_line)));
            }

            if (options.show_track_sizes) {
                for (auto const& column_track : fragment.columns.tracks) {
                    auto track_rect = CSSPixelRect {
                        origin.x() + column_track.start,
                        content_rect.y(),
                        column_track.breadth,
                        min(content_rect.height(), label_height + 4),
                    };
                    paint_centered_label(track_rect, Utf16String::from_utf8(track_size_label(column_track)));
                }

                for (auto const& row_track : fragment.rows.tracks) {
                    auto track_rect = CSSPixelRect {
                        content_rect.x(),
                        origin.y() + row_track.start,
                        min(content_rect.width(), CSSPixels(72)),
                        row_track.breadth,
                    };
                    paint_centered_label(track_rect, Utf16String::from_utf8(track_size_label(row_track)));
                }
            }

            if (options.show_area_names) {
                for (auto const& area : fragment.areas) {
                    auto column_start = line_start_for_number(fragment.columns, area.column_start);
                    auto column_end = line_start_for_number(fragment.columns, area.column_end);
                    auto row_start = line_start_for_number(fragment.rows, area.row_start);
                    auto row_end = line_start_for_number(fragment.rows, area.row_end);
                    if (!column_start.has_value() || !column_end.has_value() || !row_start.has_value() || !row_end.has_value())
                        continue;

                    auto area_rect = CSSPixelRect {
                        origin.x() + column_start.value(),
                        origin.y() + row_start.value(),
                        column_end.value() - column_start.value(),
                        row_end.value() - row_start.value(),
                    };
                    paint_rect(area_rect, color.with_alpha(24));

                    auto visible_area_rect = area_rect.intersected(viewport_rect);
                    if (!visible_area_rect.is_empty())
                        paint_centered_label(visible_area_rect, Utf16String::from_utf8(area.name));
                }
            }
        }
    });
}

void PaintableBox::paint_flexbox_inspector_overlay(DisplayListRecordingContext& context, FlexboxInspectorOverlayOptions const& options) const
{
    if (!m_flex_layout_data)
        return;

    paint_with_inspector_overlay_context(context, [&] {
        auto content_rect = absolute_united_content_rect();
        auto const origin = content_rect.location();
        auto const viewport_rect = document().viewport_rect();
        auto const& color = options.color;
        auto line_color = color.with_alpha(220);
        auto container_fill_color = color.with_alpha(28);
        auto line_fill_color = color.with_alpha(18);
        auto item_fill_color = color.with_alpha(32);
        auto line_thickness = CSSPixels(1);
        auto main_axis_is_horizontal = m_flex_layout_data->flex_direction == CSS::FlexDirection::Row
            || m_flex_layout_data->flex_direction == CSS::FlexDirection::RowReverse;

        auto paint_rect = [&](CSSPixelRect const& rect, Gfx::Color rect_color) {
            auto visible_rect = rect.intersected(viewport_rect);
            if (visible_rect.is_empty())
                return;
            context.display_list_recorder().fill_rect(context.enclosing_device_rect(visible_rect).to_type<int>(), rect_color);
        };

        auto paint_outline = [&](CSSPixelRect const& rect, Gfx::Color rect_color) {
            auto visible_rect = rect.intersected(viewport_rect);
            if (visible_rect.is_empty())
                return;
            context.display_list_recorder().draw_rect(context.enclosing_device_rect(visible_rect).to_type<int>(), rect_color);
        };

        paint_rect(content_rect, container_fill_color);
        paint_outline(content_rect, line_color);

        for (auto const& line : m_flex_layout_data->lines) {
            auto line_rect = main_axis_is_horizontal
                ? CSSPixelRect { content_rect.x(), origin.y() + line.cross_start, content_rect.width(), line.cross_size }
                : CSSPixelRect { origin.x() + line.cross_start, content_rect.y(), line.cross_size, content_rect.height() };

            paint_rect(line_rect, line_fill_color);
            paint_outline(line_rect, line_color);

            for (auto const& item : line.items) {
                auto item_rect = item.rect.translated(origin);
                paint_rect(item_rect, item_fill_color);
                paint_outline(item_rect, line_color);
            }
        }

        // Repaint the container border last so adjacent flex lines and items do not obscure it.
        paint_outline(content_rect, line_color);

        if (main_axis_is_horizontal) {
            for (auto const& line : m_flex_layout_data->lines) {
                auto y = origin.y() + line.cross_start;
                paint_rect({ content_rect.x(), y, content_rect.width(), line_thickness }, line_color);
                paint_rect({ content_rect.x(), y + line.cross_size, content_rect.width(), line_thickness }, line_color);
            }
        } else {
            for (auto const& line : m_flex_layout_data->lines) {
                auto x = origin.x() + line.cross_start;
                paint_rect({ x, content_rect.y(), line_thickness, content_rect.height() }, line_color);
                paint_rect({ x + line.cross_size, content_rect.y(), line_thickness, content_rect.height() }, line_color);
            }
        }
    });
}

void PaintableBox::set_stacking_context(NonnullRefPtr<StackingContext> stacking_context)
{
    m_stacking_context = move(stacking_context);
}

RefPtr<StackingContext> PaintableBox::stacking_context()
{
    return m_stacking_context;
}

RefPtr<StackingContext const> PaintableBox::stacking_context() const
{
    return m_stacking_context;
}

void PaintableBox::invalidate_stacking_context()
{
    m_stacking_context = nullptr;
}

Optional<int> PaintableBox::effective_z_index() const
{
    // https://drafts.csswg.org/css2/#z-index
    // Applies to: positioned elements
    if (is_positioned())
        return computed_values().z_index();

    return {};
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
        .top = box_model().border.top == 0 || m_fragment_top_edge_away ? CSS::BorderData() : computed_values().border_top(),
        .right = box_model().border.right == 0 || m_fragment_right_edge_away ? CSS::BorderData() : computed_values().border_right(),
        .bottom = box_model().border.bottom == 0 || m_fragment_bottom_edge_away ? CSS::BorderData() : computed_values().border_bottom(),
        .left = box_model().border.left == 0 || m_fragment_left_edge_away ? CSS::BorderData() : computed_values().border_left(),
    };
    paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(absolute_border_box_rect()), normalized_border_radii_data().as_corners(context.device_pixel_converter()), borders_data.to_device_pixels(context));
}

void PaintableBox::paint_backdrop_filter(DisplayListRecordingContext& context) const
{
    if (!computed_values().backdrop_filter().has_filters())
        return;

    auto resolved = resolve_css_filter(computed_values().backdrop_filter(), *this);
    auto backdrop_region = context.rounded_device_rect(absolute_border_box_rect());
    auto border_radii_data = normalized_border_radii_data();
    ScopedCornerRadiusClip corner_clipper { context, backdrop_region, border_radii_data };
    if (auto gfx_filter = to_gfx_filter(resolved, context.device_pixels_per_css_pixel()); gfx_filter.has_value())
        context.display_list_recorder().apply_backdrop_filter(backdrop_region.to_type<int>(), border_radii_data.as_corners(context.device_pixel_converter()), *gfx_filter);
}

void PaintableBox::paint_background(DisplayListRecordingContext& context) const
{
    // If the body's background properties were propagated to the root element, do not re-paint the body's background.
    if (layout_node_with_style_and_box_metrics().is_body() && document().html_element()->should_use_body_background_properties())
        return;

    auto const& computed_values = this->computed_values();

    CSSPixelRect background_rect;
    Color background_color = computed_values.background_color();
    auto const* background_layers = &computed_values.background_layers();

    // https://drafts.csswg.org/css-backgrounds/#root-background
    auto is_root = layout_node_with_style_and_box_metrics().is_root_element();
    if (is_root) {
        background_rect = absolute_border_box_rect();

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

    auto border_radii = normalized_border_radii_data();

    ResolvedBackground resolved_background;
    if (background_layers)
        resolved_background = resolve_background_layers(*background_layers, *this, background_color, computed_values.background_color_clip(), background_rect, border_radii);

    if (is_root) {
        auto canvas_rect = navigable()->viewport_rect();
        if (auto overflow_rect = scrollable_overflow_rect(); overflow_rect.has_value())
            canvas_rect.unite(overflow_rect.value());
        resolved_background.background_rect.unite(canvas_rect);
        resolved_background.color_box.rect.unite(canvas_rect);
    }

    // If the body's background was propagated to the root element, use the body's image-rendering value.
    auto image_rendering = computed_values.image_rendering();
    if (layout_node().is_root_element()
        && document().html_element()
        && document().html_element()->should_use_body_background_properties()) {
        image_rendering = document().background_image_rendering();
    }

    Painting::paint_background(context, *this, image_rendering, resolved_background, border_radii);
}

void PaintableBox::paint_box_shadow(DisplayListRecordingContext& context) const
{
    auto const& box_shadow_layers = computed_values().box_shadow();
    if (box_shadow_layers.is_empty())
        return;
    Vector<Painting::ShadowData> resolved_box_shadow_data;
    resolved_box_shadow_data.ensure_capacity(box_shadow_layers.size());
    for (auto const& layer : box_shadow_layers)
        resolved_box_shadow_data.unchecked_append(ShadowData::from_css(layer, layout_node()));
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
        border_radii_data.shrink(
            m_fragment_top_edge_away ? 0 : computed_values().border_top().width,
            m_fragment_right_edge_away ? 0 : computed_values().border_right().width,
            m_fragment_bottom_edge_away ? 0 : computed_values().border_bottom().width,
            m_fragment_left_edge_away ? 0 : computed_values().border_left().width);

    return border_radii_data;
}

Optional<CSSPixelPoint> PaintableBox::transform_point_to_local(CSSPixelPoint screen_position) const
{
    auto viewport_paintable = document().paintable();
    if (!viewport_paintable || !viewport_paintable->has_visual_context_tree())
        return screen_position;
    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& scroll_state = viewport_paintable->scroll_state_snapshot();
    auto const& visual_context_tree = viewport_paintable->visual_context_tree();
    auto result = visual_context_tree.transform_point_for_hit_test(m_accumulated_visual_context_index, screen_position.to_type<float>() * pixel_ratio, scroll_state);
    if (!result.has_value())
        return {};
    return (*result / pixel_ratio).to_type<CSSPixels>();
}

Optional<CSSPixelPoint> PaintableBox::transform_point_to_local_for_descendants(CSSPixelPoint screen_position) const
{
    auto viewport_paintable = document().paintable();
    if (!viewport_paintable || !viewport_paintable->has_visual_context_tree())
        return screen_position;
    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& scroll_state = viewport_paintable->scroll_state_snapshot();
    auto const& visual_context_tree = viewport_paintable->visual_context_tree();
    auto result = visual_context_tree.transform_point_for_hit_test(m_accumulated_visual_context_for_descendants_index, screen_position.to_type<float>() * pixel_ratio, scroll_state);
    if (!result.has_value())
        return {};
    return (*result / pixel_ratio).to_type<CSSPixels>();
}

CSSPixelRect PaintableBox::transform_rect_to_viewport(CSSPixelRect const& rect) const
{
    auto viewport_paintable = document().paintable();
    if (!viewport_paintable || !viewport_paintable->has_visual_context_tree())
        return rect;
    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& scroll_state = viewport_paintable->scroll_state_snapshot();
    auto const& visual_context_tree = viewport_paintable->visual_context_tree();
    auto result = visual_context_tree.transform_rect_to_viewport(m_accumulated_visual_context_index, rect.to_type<float>() * pixel_ratio, scroll_state);
    return (result * (1.f / pixel_ratio)).to_type<CSSPixels>();
}

CSSPixelPoint PaintableBox::inverse_transform_point(CSSPixelPoint screen_position) const
{
    auto viewport_paintable = document().paintable();
    if (!viewport_paintable || !viewport_paintable->has_visual_context_tree())
        return screen_position;
    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& visual_context_tree = viewport_paintable->visual_context_tree();
    auto result = visual_context_tree.inverse_transform_point(m_accumulated_visual_context_index, screen_position.to_type<float>() * pixel_ratio);
    return (result / pixel_ratio).to_type<CSSPixels>();
}

CSSPixelPoint PaintableBox::transform_to_local_coordinates(CSSPixelPoint screen_position) const
{
    return transform_point_to_local(screen_position).value_or(screen_position);
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

    auto axes = physical_resize_axes();
    return axes.horizontal || axes.vertical;
}

bool PaintableBox::is_chrome_mirrored() const
{
    auto const& writing_mode = computed_values().writing_mode();
    return (writing_mode == CSS::WritingMode::HorizontalTb && computed_values().direction() == CSS::Direction::Rtl)
        || writing_mode == CSS::WritingMode::VerticalRl
        || writing_mode == CSS::WritingMode::SidewaysRl;
}

RefPtr<ResizeHandle> PaintableBox::resize_handle() const
{
    return m_resize_handle;
}

NonnullRefPtr<ResizeHandle> PaintableBox::ensure_resize_handle()
{
    if (!m_resize_handle)
        m_resize_handle = ResizeHandle::create(*this);
    return *m_resize_handle;
}

bool PaintableBox::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, double wheel_delta_x, double wheel_delta_y)
{
    auto can_scroll_horizontally = could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal);
    auto can_scroll_vertically = could_be_scrolled_by_wheel_event(ScrollDirection::Vertical);
    if (!can_scroll_horizontally)
        wheel_delta_x = 0;
    if (!can_scroll_vertically)
        wheel_delta_y = 0;

    // if none of the axes we scrolled with can be accepted by this element, don't handle scroll.
    if (wheel_delta_x == 0 && wheel_delta_y == 0)
        return false;

    auto scroll_handled = scroll_by(wheel_delta_x, wheel_delta_y);
    return scroll_handled == ScrollHandled::Yes;
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

void PaintableBox::set_needs_repaint(InvalidateDisplayList should_invalidate_display_list)
{
    if (should_invalidate_display_list == InvalidateDisplayList::Yes) {
        invalidate_paint_cache();

        // Recurse into anonymous child nodes so we properly invalidate nested contents of e.g. <button>s.
        for_each_child_of_type<PaintableBox>([&](auto& child) {
            if (child.layout_node().is_anonymous())
                child.set_needs_repaint(should_invalidate_display_list);
            return IterationDecision::Continue;
        });
    }
    Paintable::set_needs_repaint(should_invalidate_display_list);
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
        auto svg_paintable = first_ancestor_of_type<Painting::SVGSVGPaintable>();
        if (!svg_paintable)
            return absolute_border_box_rect();
        return svg_paintable->absolute_rect();
    }
    VERIFY_NOT_REACHED();
}

BorderRadiiData PaintableBox::border_radii_data() const
{
    auto const& computed_values = this->computed_values();
    if (!computed_values.has_noninitial_border_radii())
        return {};
    CSSPixelRect const border_rect { 0, 0, border_box_width(), border_box_height() };
    auto border_top_left_radius = m_fragment_top_edge_away || m_fragment_left_edge_away ? CSS::BorderRadiusData {} : computed_values.border_top_left_radius();
    auto border_top_right_radius = m_fragment_top_edge_away || m_fragment_right_edge_away ? CSS::BorderRadiusData {} : computed_values.border_top_right_radius();
    auto border_bottom_right_radius = m_fragment_bottom_edge_away || m_fragment_right_edge_away ? CSS::BorderRadiusData {} : computed_values.border_bottom_right_radius();
    auto border_bottom_left_radius = m_fragment_bottom_edge_away || m_fragment_left_edge_away ? CSS::BorderRadiusData {} : computed_values.border_bottom_left_radius();
    return normalize_border_radii_data(layout_node(), border_rect, border_rect,
        border_top_left_radius, border_top_right_radius,
        border_bottom_right_radius, border_bottom_left_radius);
}

Optional<BordersData> PaintableBox::outline_data() const
{
    auto const& computed_values = this->computed_values();
    return borders_data_for_outline(layout_node(), computed_values.outline_color(), computed_values.outline_style(), computed_values.outline_width());
}

CSSPixels PaintableBox::outline_offset() const
{
    return computed_values().outline_offset().to_px(layout_node());
}

ScrollFrameIndex PaintableBox::nearest_scroll_frame_index() const
{
    if (is_fixed_position())
        return {};
    auto paintable = this->containing_block();
    while (paintable) {
        if (paintable->own_scroll_frame_index().value())
            return paintable->own_scroll_frame_index();
        // Sticky elements need to find a scroll container even through fixed-position ancestors,
        // because they must reference a scrollport for their sticky offset computation.
        if (paintable->is_fixed_position() && !is_sticky_position())
            return {};
        paintable = paintable->containing_block();
    }
    return {};
}

RefPtr<PaintableBox const> PaintableBox::nearest_scrollable_ancestor() const
{
    auto paintable = this->containing_block();
    while (paintable) {
        if (paintable->could_be_scrolled_by_wheel_event())
            return paintable;
        if (paintable->is_fixed_position())
            return nullptr;
        paintable = paintable->containing_block();
    }
    return nullptr;
}

PaintableBox::PhysicalResizeAxes PaintableBox::physical_resize_axes() const
{
    auto const& computed = computed_values();

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
