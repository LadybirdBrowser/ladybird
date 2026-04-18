/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/ShadowPainting.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(PaintableWithLines);

static void paint_text_decoration(DisplayListRecordingContext&, TextPaintable const&, PaintableFragment::FragmentSpan const&);
static Gfx::Path build_triangle_wave_path(Gfx::IntPoint from, Gfx::IntPoint to, float amplitude);
static void compute_render_spans(PaintableFragment const&, Vector<PaintableFragment::FragmentSpan, 4>&);
static void paint_text_fragment(DisplayListRecordingContext&, PaintableFragment::FragmentSpan const&);

GC::Ref<PaintableWithLines> PaintableWithLines::create(Layout::BlockContainer const& block_container)
{
    return block_container.heap().allocate<PaintableWithLines>(block_container);
}

GC::Ref<PaintableWithLines> PaintableWithLines::create(Layout::InlineNode const& inline_node, size_t line_index)
{
    return inline_node.heap().allocate<PaintableWithLines>(inline_node, line_index);
}

PaintableWithLines::PaintableWithLines(Layout::BlockContainer const& layout_box)
    : PaintableBox(layout_box)
{
}

PaintableWithLines::PaintableWithLines(Layout::InlineNode const& inline_node, size_t line_index)
    : PaintableBox(inline_node)
    , m_line_index(line_index)
{
}

PaintableWithLines::~PaintableWithLines()
{
}

void PaintableWithLines::reset_for_relayout()
{
    PaintableBox::reset_for_relayout();
    m_fragments.clear();
}

void PaintableWithLines::paint_text_fragment_debug_highlight(DisplayListRecordingContext& context, PaintableFragment const& fragment)
{
    auto fragment_absolute_rect = fragment.absolute_rect();
    auto fragment_absolute_device_rect = context.enclosing_device_rect(fragment_absolute_rect);
    context.display_list_recorder().draw_rect(fragment_absolute_device_rect.to_type<int>(), Color::Green);

    auto baseline_start = context.rounded_device_point(fragment_absolute_rect.top_left().translated(0, fragment.baseline())).to_type<int>();
    auto baseline_end = context.rounded_device_point(fragment_absolute_rect.top_right().translated(-1, fragment.baseline())).to_type<int>();
    context.display_list_recorder().draw_line(baseline_start, baseline_end, Color::Red);
}

TraversalDecision PaintableWithLines::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    auto const is_visible = computed_values().visibility() == CSS::Visibility::Visible;

    Optional<CSSPixelPoint> local_position;
    bool acquired_local_position = false;

    auto ensure_local_position = [&]() {
        if (exchange(acquired_local_position, true))
            return;
        local_position = transform_point_to_local(position);
    };

    // TextCursor hit testing mode should be able to place cursor in contenteditable elements even if they are empty.
    if (m_fragments.is_empty()
        && !has_children()
        && type == HitTestType::TextCursor
        && layout_node().dom_node()
        && layout_node().dom_node()->is_editable_or_editing_host()
        && is_visible
        && visible_for_hit_testing()) {
        ensure_local_position();

        if (local_position.has_value() && absolute_border_box_rect().contains(*local_position)) {
            HitTestResult const hit_test_result {
                .paintable = const_cast<PaintableWithLines&>(*this),
                .index_in_node = 0,
                .vertical_distance = 0,
                .horizontal_distance = 0,
            };

            if (callback(hit_test_result) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }
    }

    if (!layout_node().children_are_inline())
        return PaintableBox::hit_test(position, type, callback);

    // Only hit test chrome for visible elements.
    if (is_visible) {
        if (hit_test_chrome(position, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    if (hit_test_children(position, type, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    // Hidden elements and elements with pointer-events: none shouldn't be hit.
    if (!is_visible || !visible_for_hit_testing())
        return TraversalDecision::Continue;

    ensure_local_position();
    if (!local_position.has_value())
        return TraversalDecision::Continue;

    // Fragments are descendants of this element, so use the descendants' visual context to account for this element's
    // own scroll offset during fragment hit testing.
    Optional<CSSPixelPoint> local_position_for_fragments = transform_point_to_local_for_descendants(position);
    if (local_position_for_fragments.has_value()) {
        if (hit_test_fragments(position, local_position_for_fragments.value(), type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    if (!stacking_context() && (!layout_node().is_anonymous() || is_positioned())
        && absolute_border_box_rect().contains(local_position.value())) {
        if (callback(HitTestResult { .paintable = const_cast<PaintableWithLines&>(*this) }) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

TraversalDecision PaintableWithLines::hit_test_fragments(CSSPixelPoint position, CSSPixelPoint local_position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    for (auto const& fragment : fragments()) {
        if (fragment.paintable().has_stacking_context() || !fragment.paintable().is_visible() || !fragment.paintable().visible_for_hit_testing())
            continue;
        auto fragment_absolute_rect = fragment.absolute_rect();
        if (fragment_absolute_rect.contains(local_position)) {
            if (fragment.paintable().hit_test(position, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
            HitTestResult hit_test_result { .paintable = const_cast<Paintable&>(fragment.paintable()), .index_in_node = fragment.index_in_node_for_point(local_position), .vertical_distance = 0, .horizontal_distance = 0 };
            if (callback(hit_test_result) == TraversalDecision::Break)
                return TraversalDecision::Break;
        } else if (type == HitTestType::TextCursor) {
            auto const* common_ancestor_parent = [&]() -> DOM::Node const* {
                auto selection = document().get_selection();
                if (!selection)
                    return nullptr;
                auto range = selection->range();
                if (!range)
                    return nullptr;
                auto common_ancestor = range->common_ancestor_container();
                if (common_ancestor->parent())
                    return common_ancestor->parent();
                return common_ancestor;
            }();

            // If we reached this point, the position is not within the fragment. However, the fragment start or end might be
            // the place to place the cursor, so long as it does not have user-select: none.
            if (fragment.layout_node().user_select_used_value() == CSS::UserSelect::None)
                continue;

            auto const* fragment_dom_node = fragment.layout_node().dom_node();
            if (common_ancestor_parent && fragment_dom_node && common_ancestor_parent->is_ancestor_of(*fragment_dom_node)) {
                // To determine the best place, we first find the closest fragment horizontally to the cursor. If we could not
                // find one, then find for the closest vertically above the cursor. If we knew the direction of selection, we
                // would look above if selecting upward.
                HitTestResult hit_test_result {
                    .paintable = const_cast<Paintable&>(fragment.paintable()),
                    .index_in_node = fragment.start_offset(),
                };
                if (fragment_absolute_rect.bottom() - 1 <= local_position.y()) { // fully below the fragment
                    hit_test_result.index_in_node += fragment.length_in_code_units();
                    hit_test_result.vertical_distance = local_position.y() - fragment_absolute_rect.bottom();
                } else if (local_position.y() < fragment_absolute_rect.top()) { // fully above the fragment
                    hit_test_result.vertical_distance = fragment_absolute_rect.top() - local_position.y();
                } else { // vertically within the fragment
                    hit_test_result.vertical_distance = 0;
                    if (local_position.x() < fragment_absolute_rect.left()) {
                        hit_test_result.horizontal_distance = fragment_absolute_rect.left() - local_position.x();
                    } else {
                        hit_test_result.index_in_node += fragment.length_in_code_units();
                        hit_test_result.horizontal_distance = local_position.x() - fragment_absolute_rect.right();
                    }
                }

                if (callback(hit_test_result) == TraversalDecision::Break)
                    return TraversalDecision::Break;
            }
        }
    }
    return TraversalDecision::Continue;
}

static void resolve_text_fragment_properties(PaintableWithLines const& paintable_with_lines)
{
    auto const& parent_layout_node = paintable_with_lines.layout_node();
    for (auto& fragment : const_cast<PaintableWithLines&>(paintable_with_lines).fragments()) {
        auto const* text_node = as_if<Layout::TextNode>(fragment.layout_node());
        if (!text_node)
            continue;

        auto const& font = text_node->first_available_font();
        auto const glyph_height = CSSPixels::nearest_value_for(font.pixel_size());
        auto const line_thickness = [&] {
            auto const& thickness = text_node->computed_values().text_decoration_thickness();
            return thickness.value.visit(
                [glyph_height](CSS::TextDecorationThickness::Auto) {
                    // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-auto
                    // The UA chooses an appropriate thickness for text decoration lines; see below.
                    return max(glyph_height.scaled(0.1), 1);
                },
                [glyph_height](CSS::TextDecorationThickness::FromFont) {
                    // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-from-font
                    // If the first available font has metrics indicating a preferred underline width, use that width,
                    // otherwise behaves as auto.
                    // FIXME: Implement this properly.
                    return max(glyph_height.scaled(0.1), 1);
                },
                [&](CSS::LengthPercentage const& length_percentage) {
                    // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-length-percentage
                    auto resolved_length = length_percentage.resolved(*text_node, CSS::Length(1, CSS::LengthUnit::Em).to_px(*text_node)).to_px(*text_node);
                    return max(resolved_length, 1);
                });
        }();
        fragment.set_text_decoration_thickness(line_thickness);

        auto const& text_shadow = text_node->computed_values().text_shadow();
        Vector<ShadowData> resolved_shadow_data;
        if (!text_shadow.is_empty()) {
            resolved_shadow_data.ensure_capacity(text_shadow.size());
            for (auto const& layer : text_shadow)
                resolved_shadow_data.append(ShadowData::from_css(layer, parent_layout_node));
        }
        fragment.set_shadows(move(resolved_shadow_data));
    }
}

void PaintableWithLines::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        resolve_text_fragment_properties(*this);

        Vector<PaintableFragment::FragmentSpan, 4> spans;
        for (auto const& fragment : m_fragments)
            compute_render_spans(fragment, spans);

        for (auto const& span : spans) {
            if (span.background_color.alpha() > 0) {
                auto selection_rect = context.rounded_device_rect(span.fragment.selection_rect()).to_type<int>();
                context.display_list_recorder().fill_rect(selection_rect, span.background_color);
            }
        }

        for (auto const& span : spans)
            paint_text_shadow(context, span);

        for (auto const& span : spans)
            paint_text_fragment(context, span);

        if (document().cursor_position())
            paint_cursor(context);
    }
}

void compute_render_spans(PaintableFragment const& fragment, Vector<PaintableFragment::FragmentSpan, 4>& spans)
{
    auto const* text_paintable = as_if<TextPaintable>(fragment.paintable());
    if (!text_paintable) {
        // Non-text fragments still need shadow painting.
        spans.append({
            .fragment = fragment,
            .start_code_unit = 0,
            .end_code_unit = 0,
            .text_color = Color::Transparent,
            .background_color = Color::Transparent,
            .shadow_layers = {},
            .text_decoration = {},
        });
        return;
    }

    if (!text_paintable->is_visible())
        return;

    auto text_color = text_paintable->computed_values().webkit_text_fill_color();
    auto selection_offsets = fragment.selection_offsets();

    // No selection: single span with base styling.
    if (!selection_offsets.has_value()) {
        spans.append({
            .fragment = fragment,
            .start_code_unit = 0,
            .end_code_unit = fragment.length_in_code_units(),
            .text_color = text_color,
            .background_color = Color::Transparent,
            .shadow_layers = {},
            .text_decoration = {},
        });
        return;
    }

    auto [selection_start, selection_end, _] = *selection_offsets;
    auto selection_style = text_paintable->selection_style();
    auto selection_text_color = selection_style.text_color.value_or(text_color);

    // Convert selection text decoration to fragment text decoration data.
    Optional<PaintableFragment::TextDecorationData> selection_text_decoration;
    if (selection_style.text_decoration.has_value()) {
        selection_text_decoration = PaintableFragment::TextDecorationData {
            .line = move(selection_style.text_decoration->line),
            .style = selection_style.text_decoration->style,
            .color = selection_style.text_decoration->color,
        };
    }

    // Before selection.
    if (selection_start > 0) {
        spans.append({
            .fragment = fragment,
            .start_code_unit = 0,
            .end_code_unit = selection_start,
            .text_color = text_color,
            .background_color = Color::Transparent,
            .shadow_layers = {},
            .text_decoration = {},
        });
    }

    // Selected portion.
    if (selection_start < selection_end) {
        spans.append({
            .fragment = fragment,
            .start_code_unit = selection_start,
            .end_code_unit = selection_end,
            .text_color = selection_text_color,
            .background_color = selection_style.background_color,
            .shadow_layers = move(selection_style.text_shadow),
            .text_decoration = move(selection_text_decoration),
        });
    }

    // After selection.
    if (selection_end < fragment.length_in_code_units()) {
        spans.append({
            .fragment = fragment,
            .start_code_unit = selection_end,
            .end_code_unit = fragment.length_in_code_units(),
            .text_color = text_color,
            .background_color = Color::Transparent,
            .shadow_layers = {},
            .text_decoration = {},
        });
    }
}

void paint_text_fragment(DisplayListRecordingContext& context, PaintableFragment::FragmentSpan const& span)
{
    auto const& fragment = span.fragment;

    // Skip non-text spans (they're only for shadow painting).
    if (span.start_code_unit == span.end_code_unit)
        return;

    auto const& text_paintable = as<TextPaintable>(fragment.paintable());

    if (context.should_show_line_box_borders())
        PaintableWithLines::paint_text_fragment_debug_highlight(context, fragment);

    auto glyph_run = fragment.glyph_run();
    if (!glyph_run)
        return;

    auto& painter = context.display_list_recorder();
    auto fragment_absolute_rect = fragment.absolute_rect();
    auto fragment_device_rect = context.enclosing_device_rect(fragment_absolute_rect).to_type<int>();
    auto scale = context.device_pixels_per_css_pixel();
    auto baseline_start = Gfx::FloatPoint {
        fragment_absolute_rect.x().to_float(),
        fragment_absolute_rect.y().to_float() + fragment.baseline().to_float(),
    } * scale;

    // Paint text, clipped to span range if not full fragment.
    bool is_full_fragment = span.start_code_unit == 0 && span.end_code_unit == fragment.length_in_code_units();
    if (is_full_fragment) {
        painter.draw_glyph_run(baseline_start, *glyph_run, span.text_color, fragment_device_rect, scale, fragment.orientation());
    } else {
        auto range_rect = fragment.range_rect(Paintable::SelectionState::StartAndEnd,
            fragment.start_offset() + span.start_code_unit,
            fragment.start_offset() + span.end_code_unit);
        auto span_rect = context.rounded_device_rect(range_rect).to_type<int>();
        painter.save();
        painter.add_clip_rect(span_rect);
        painter.draw_glyph_run(baseline_start, *glyph_run, span.text_color, fragment_device_rect, scale, fragment.orientation());
        painter.restore();
    }

    paint_text_decoration(context, text_paintable, span);
}

Optional<PaintableFragment const&> PaintableWithLines::fragment_at_position(DOM::Position const& position) const
{
    return m_fragments.first_matching([&](auto const& fragment) {
        auto const* text_paintable = as_if<TextPaintable>(fragment.paintable());
        if (!text_paintable)
            return false;
        if (position.offset() < fragment.start_offset())
            return false;
        if (position.offset() > fragment.start_offset() + fragment.length_in_code_units())
            return false;
        return position.node() == text_paintable->dom_node();
    });
}

void PaintableWithLines::paint_cursor(DisplayListRecordingContext& context) const
{
    if (!document().cursor_blink_state() || !document().navigable()->is_focused())
        return;

    auto cursor_position = document().cursor_position();
    VERIFY(cursor_position);

    auto const* dom_node = layout_node().dom_node();
    if (!dom_node)
        return;

    auto active_element_is_editable = false;
    if (auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(document().active_element()))
        active_element_is_editable = text_control->text_control_to_html_element().is_mutable();
    if (!active_element_is_editable && !dom_node->is_editable_or_editing_host())
        return;

    auto fragment = fragment_at_position(*cursor_position);

    CSSPixelRect cursor_rect;
    Color caret_color;

    if (fragment.has_value()) {
        caret_color = as<TextPaintable>(fragment->paintable()).computed_values().caret_color();
        cursor_rect = fragment->range_rect(SelectionState::StartAndEnd, cursor_position->offset(), cursor_position->offset());
    } else {
        // Empty editable elements have no fragments, but should still draw a cursor.
        if (cursor_position->node() != dom_node)
            return;

        caret_color = computed_values().caret_color();
        auto content_box = absolute_padding_box_rect();
        cursor_rect = { content_box.x(), content_box.y(), 1, computed_values().line_height() };
    }

    if (caret_color.alpha() == 0)
        return;

    auto cursor_device_rect = context.rounded_device_rect(cursor_rect).to_type<int>();

    context.display_list_recorder().fill_rect(cursor_device_rect, caret_color);
}

struct DecorationSegment {
    int start_x;
    int end_x;
};

// https://drafts.csswg.org/css-text-decor-4/#text-decoration-skip-ink-property
static Vector<DecorationSegment> compute_skip_ink_segments(
    PaintableFragment const& fragment,
    DisplayListRecordingContext const& context,
    int span_start_x,
    int span_end_x,
    int line_y,
    int line_thickness,
    float font_size)
{
    auto glyph_run = fragment.glyph_run();
    if (!glyph_run)
        return { { span_start_x, span_end_x } };

    // The text blob is drawn at baseline_start on the canvas. Compute that same origin so we can convert between
    // device-pixel coordinates and blob-local coordinates.
    auto scale = context.device_pixels_per_css_pixel();
    auto fragment_absolute_rect = fragment.absolute_rect();
    float blob_origin_x = fragment_absolute_rect.x().to_float() * static_cast<float>(scale);
    float blob_origin_y = (fragment_absolute_rect.y().to_float() + fragment.baseline().to_float()) * static_cast<float>(scale);

    // Convert the underline's y-band from device pixels to blob-local coordinates.
    float half_thickness = line_thickness / 2.f;
    float y_top = line_y - half_thickness - blob_origin_y;
    float y_bottom = line_y + half_thickness - blob_origin_y;

    auto intervals = glyph_run->get_glyph_intercepts(scale, y_top, y_bottom);
    if (intervals.is_empty())
        return { { span_start_x, span_end_x } };

    // Use the full fragment's X range for gap computation so intercepts aren't cut off at span boundaries.
    auto full_fragment_rect = context.rounded_device_rect(fragment_absolute_rect);
    int fragment_start_x = full_fragment_rect.left().value();

    // Convert intercepts from blob-local x to device pixels, and dilate to create visible gaps.
    float dilation = max(font_size / 20.f, 2.f) * static_cast<float>(scale);

    Vector<DecorationSegment> segments;
    int current_x = fragment_start_x;
    for (size_t i = 0; i + 1 < intervals.size(); i += 2) {
        int gap_start = static_cast<int>(floorf(intervals[i] + blob_origin_x - dilation));
        int gap_end = static_cast<int>(ceilf(intervals[i + 1] + blob_origin_x + dilation));

        int seg_start = max(current_x, span_start_x);
        int seg_end = min(gap_start, span_end_x);
        if (seg_start < seg_end)
            segments.append({ seg_start, seg_end });
        current_x = max(gap_end, current_x);
    }

    int seg_start = max(current_x, span_start_x);
    if (seg_start < span_end_x)
        segments.append({ seg_start, span_end_x });

    return segments;
}

void paint_text_decoration(DisplayListRecordingContext& context, TextPaintable const& paintable, PaintableFragment::FragmentSpan const& span)
{
    auto const& fragment = span.fragment;
    auto& recorder = context.display_list_recorder();
    auto& font = fragment.layout_node().first_available_font();
    CSSPixels glyph_height = CSSPixels::nearest_value_for(font.pixel_size());
    auto baseline = fragment.baseline();

    // Use span's text decoration if explicitly set, otherwise use the element's computed values.
    Color line_color;
    CSS::TextDecorationStyle line_style;
    Vector<CSS::TextDecorationLine> text_decoration_lines;
    if (span.text_decoration.has_value()) {
        line_color = span.text_decoration->color;
        line_style = span.text_decoration->style;
        text_decoration_lines = span.text_decoration->line;
    } else {
        line_color = paintable.computed_values().text_decoration_color();
        line_style = paintable.computed_values().text_decoration_style();
        text_decoration_lines = paintable.computed_values().text_decoration_line();
    }

    // Compute the decoration box for this span.
    auto fragment_box = fragment.absolute_rect();
    if (span.start_code_unit != 0 || span.end_code_unit != fragment.length_in_code_units()) {
        auto span_rect = fragment.range_rect(Paintable::SelectionState::StartAndEnd,
            fragment.start_offset() + span.start_code_unit,
            fragment.start_offset() + span.end_code_unit);
        fragment_box.set_x(span_rect.x());
        fragment_box.set_width(span_rect.width());
    }
    auto text_underline_offset = paintable.computed_values().text_underline_offset();
    auto text_underline_position = paintable.computed_values().text_underline_position();
    for (auto line : text_decoration_lines) {
        auto line_thickness = fragment.text_decoration_thickness();

        if (line == CSS::TextDecorationLine::SpellingError) {
            // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-line-spelling-error
            // This value indicates the type of text decoration used by the user agent to highlight spelling mistakes.
            // Its appearance is UA-defined, and may be platform-dependent. It is often rendered as a red wavy underline.
            line_color = Color::Red;
            line_thickness = CSSPixels(1);
            line_style = CSS::TextDecorationStyle::Wavy;
            line = CSS::TextDecorationLine::Underline;

            // https://drafts.csswg.org/css-text-decor-4/#underline-offset
            // When the value of the text-decoration-line property is either spelling-error or grammar-error, the UA
            // must ignore the value of text-underline-position.
            text_underline_offset = CSS::InitialValues::text_underline_offset();
        } else if (line == CSS::TextDecorationLine::GrammarError) {
            // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-line-grammar-error
            // This value indicates the type of text decoration used by the user agent to highlight grammar mistakes.
            // Its appearance is UA defined, and may be platform-dependent. It is often rendered as a green wavy underline.
            line_color = Color::DarkGreen;
            line_thickness = CSSPixels(1);
            line_style = CSS::TextDecorationStyle::Wavy;
            line = CSS::TextDecorationLine::Underline;

            // https://drafts.csswg.org/css-text-decor-4/#underline-offset
            // When the value of the text-decoration-line property is either spelling-error or grammar-error, the UA
            // must ignore the value of text-underline-position.
            text_underline_offset = CSS::InitialValues::text_underline_offset();
        }

        auto device_line_thickness = context.rounded_device_pixels(line_thickness);

        // Compute the center Y of the decoration stroke. For underline and overline, offset by half the thickness
        // so the near edge of the stroke aligns with the intended position.
        CSSPixels line_center_y;
        switch (line) {
        case CSS::TextDecorationLine::None:
            return;
        case CSS::TextDecorationLine::Underline: {
            // https://drafts.csswg.org/css-text-decor-4/#text-underline-position-property
            auto underline_top_edge = [&]() {
                // FIXME: Support text-decoration: underline on vertical text
                switch (text_underline_position.horizontal) {
                case CSS::TextUnderlinePositionHorizontal::Auto:
                    // The user agent may use any algorithm to determine the underline’s position; however it must be
                    // placed at or under the alphabetic baseline.

                    // Spec Note: It is suggested that the default underline position be close to the alphabetic
                    //            baseline,
                    // FIXME:     unless that would either cross subscripted (or otherwise lowered) text or draw over
                    //            glyphs from Asian scripts such as Han or Tibetan for which an alphabetic underline is
                    //            too high: in such cases, shifting the underline lower or aligning to the em box edge
                    //            as described for under may be more appropriate.
                    return fragment.baseline() + text_underline_offset;
                case CSS::TextUnderlinePositionHorizontal::FromFont:
                    // FIXME: If the first available font has metrics indicating a preferred underline offset, use that
                    //        offset, otherwise behaves as auto.
                    return fragment.baseline() + text_underline_offset;
                case CSS::TextUnderlinePositionHorizontal::Under:
                    // The underline is positioned under the element’s text content. In this case the underline usually
                    // does not cross the descenders. (This is sometimes called “accounting” underline.)
                    return fragment.baseline() + CSSPixels { font.pixel_metrics().descent } + text_underline_offset;
                }
                VERIFY_NOT_REACHED();
            }();
            line_center_y = underline_top_edge + line_thickness / 2;
            break;
        }
        case CSS::TextDecorationLine::Overline:
            line_center_y = baseline - glyph_height - line_thickness / 2;
            break;
        case CSS::TextDecorationLine::LineThrough: {
            auto x_height = font.x_height();
            line_center_y = baseline - x_height * CSSPixels(0.5f);
            break;
        }
        case CSS::TextDecorationLine::Blink:
            // Conforming user agents may simply not blink the text
            return;
        case CSS::TextDecorationLine::SpellingError:
        case CSS::TextDecorationLine::GrammarError:
            // Handled above.
            VERIFY_NOT_REACHED();
        }

        auto line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, line_center_y));
        auto line_end_point = context.rounded_device_point(fragment_box.top_right().translated(0, line_center_y));

        // https://drafts.csswg.org/css-text-decor-4/#text-decoration-skip-ink-property
        // FIXME: For text-decoration-skip-ink: auto, skip CJK ideographs and symbols from the intercept
        //        computation, since their complex strokes would create too many gaps in the decoration line.
        auto skip_ink = paintable.computed_values().text_decoration_skip_ink();
        bool should_skip_ink = skip_ink != CSS::TextDecorationSkipInk::None
            && first_is_one_of(line, CSS::TextDecorationLine::Underline, CSS::TextDecorationLine::Overline);

        auto draw_line_for_segment = [&](DecorationSegment segment, int y, Gfx::LineStyle style = Gfx::LineStyle::Solid) {
            recorder.draw_line({ segment.start_x, y }, { segment.end_x, y }, line_color, device_line_thickness.value(), style);
        };

        auto segments = [&] -> Vector<DecorationSegment> {
            if (!should_skip_ink)
                return { { line_start_point.x().value(), line_end_point.x().value() } };
            return compute_skip_ink_segments(fragment, context, line_start_point.x().value(), line_end_point.x().value(),
                line_start_point.y().value(), device_line_thickness.value(), font.pixel_size());
        }();

        auto line_y = line_start_point.y().value();

        switch (line_style) {
        case CSS::TextDecorationStyle::Solid:
            for (auto segment : segments)
                draw_line_for_segment(segment, line_y);
            break;
        case CSS::TextDecorationStyle::Double: {
            // Two parallel lines with a 1px gap, expanding away from the text.
            int step = device_line_thickness.value() + 1;
            int first_y = line_y;
            int second_y = line_y;
            switch (line) {
            case CSS::TextDecorationLine::Underline:
                second_y += step;
                break;
            case CSS::TextDecorationLine::Overline:
                second_y -= step;
                break;
            case CSS::TextDecorationLine::LineThrough:
                first_y -= step / 2;
                second_y = first_y + step;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
            for (auto segment : segments) {
                draw_line_for_segment(segment, first_y);
                draw_line_for_segment(segment, second_y);
            }
            break;
        }
        case CSS::TextDecorationStyle::Dashed:
            for (auto segment : segments)
                draw_line_for_segment(segment, line_y, Gfx::LineStyle::Dashed);
            break;
        case CSS::TextDecorationStyle::Dotted:
            for (auto segment : segments)
                draw_line_for_segment(segment, line_y, Gfx::LineStyle::Dotted);
            break;
        case CSS::TextDecorationStyle::Wavy: {
            // The wave oscillates amplitude/2 above and below its center, so shift the center away from the text so the
            // near peaks don’t overlap it.
            int amplitude = device_line_thickness.value() * 3;
            int wave_y = line_y;
            switch (line) {
            case CSS::TextDecorationLine::Underline:
                wave_y += device_line_thickness.value() / 2 + 1;
                break;
            case CSS::TextDecorationLine::Overline:
                wave_y -= device_line_thickness.value() / 2 + 1;
                break;
            case CSS::TextDecorationLine::LineThrough:
                break;
            default:
                VERIFY_NOT_REACHED();
            }
            for (auto segment : segments) {
                Gfx::IntPoint from { segment.start_x, wave_y };
                Gfx::IntPoint to { segment.end_x, wave_y };
                recorder.stroke_path({
                    .cap_style = Gfx::Path::CapStyle::Round,
                    .join_style = Gfx::Path::JoinStyle::Round,
                    .miter_limit = 0,
                    .dash_array = {},
                    .dash_offset = 0,
                    .path = build_triangle_wave_path(from, to, amplitude),
                    .paint_style_or_color = line_color,
                    .thickness = static_cast<float>(device_line_thickness.value()),
                });
            }
            break;
        }
        }
    }
}

Gfx::Path build_triangle_wave_path(Gfx::IntPoint from, Gfx::IntPoint to, float amplitude)
{
    Gfx::Path path;
    if (from.y() != to.y()) {
        dbgln("FIXME: Support more than horizontal waves");
        return path;
    }

    path.move_to(from.to_type<float>());

    float const wavelength = amplitude * 2.0f;
    float const half_wavelength = amplitude;
    float const quarter_wavelength = amplitude / 2.0f;

    auto position = from.to_type<float>();
    auto remaining = abs(to.x() - position.x());
    while (remaining > wavelength) {
        // Draw a whole wave
        path.line_to({ position.x() + quarter_wavelength, position.y() - quarter_wavelength });
        path.line_to({ position.x() + quarter_wavelength + half_wavelength, position.y() + quarter_wavelength });
        path.line_to({ position.x() + wavelength, (float)position.y() });
        position.translate_by({ wavelength, 0 });
        remaining = abs(to.x() - position.x());
    }

    // Up
    if (remaining > quarter_wavelength) {
        path.line_to({ position.x() + quarter_wavelength, position.y() - quarter_wavelength });
        position.translate_by({ quarter_wavelength, 0 });
        remaining = abs(to.x() - position.x());
    } else if (remaining >= 1) {
        auto fraction = remaining / quarter_wavelength;
        path.line_to({ position.x() + (fraction * quarter_wavelength), position.y() - (fraction * quarter_wavelength) });
        remaining = 0;
    }

    // Down
    if (remaining > half_wavelength) {
        path.line_to({ position.x() + half_wavelength, position.y() + quarter_wavelength });
        position.translate_by(half_wavelength, 0);
        remaining = abs(to.x() - position.x());
    } else if (remaining >= 1) {
        auto fraction = remaining / half_wavelength;
        path.line_to({ position.x() + (fraction * half_wavelength), position.y() - quarter_wavelength + (fraction * half_wavelength) });
        remaining = 0;
    }

    // Back to middle
    if (remaining >= 1) {
        auto fraction = remaining / quarter_wavelength;
        path.line_to({ position.x() + (fraction * quarter_wavelength), position.y() + ((1 - fraction) * quarter_wavelength) });
    }

    return path;
}

} // namespace Web::Painting
