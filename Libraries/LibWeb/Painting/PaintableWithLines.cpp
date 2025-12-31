/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/ShadowPainting.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(PaintableWithLines);

static void paint_text_decoration(DisplayListRecordingContext&, TextPaintable const&, PaintableFragment const&);
static Gfx::Path build_triangle_wave_path(Gfx::IntPoint from, Gfx::IntPoint to, float amplitude);
static void paint_text_fragment(DisplayListRecordingContext&, TextPaintable const&, PaintableFragment const&);

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
    if (clip_rect_for_hit_testing().has_value() && !clip_rect_for_hit_testing()->contains(position))
        return TraversalDecision::Continue;

    if (computed_values().visibility() != CSS::Visibility::Visible)
        return TraversalDecision::Continue;

    // TextCursor hit testing mode should be able to place cursor in contenteditable elements even if they are empty
    if (m_fragments.is_empty()
        && !has_children()
        && type == HitTestType::TextCursor
        && layout_node().dom_node()
        && layout_node().dom_node()->is_editable()) {
        HitTestResult const hit_test_result {
            .paintable = const_cast<PaintableWithLines&>(*this),
            .index_in_node = 0,
            .vertical_distance = 0,
            .horizontal_distance = 0,
        };
        if (callback(hit_test_result) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    if (!layout_node().children_are_inline())
        return PaintableBox::hit_test(position, type, callback);

    if (hit_test_scrollbars(position, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (hit_test_children(position, type, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (!visible_for_hit_testing())
        return TraversalDecision::Continue;

    auto const offset_position_adjusted_by_scroll_offset = adjust_position_for_cumulative_scroll_offset(position);
    auto const* common_ancestor_parent = [&]() -> DOM::Node const* {
        if (type != HitTestType::TextCursor)
            return nullptr;
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
    for (auto const& fragment : fragments()) {
        if (fragment.paintable().has_stacking_context() || !fragment.paintable().visible_for_hit_testing())
            continue;
        auto fragment_absolute_rect = fragment.absolute_rect();
        if (fragment_absolute_rect.contains(offset_position_adjusted_by_scroll_offset)) {
            if (fragment.paintable().hit_test(position, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
            HitTestResult hit_test_result { const_cast<Paintable&>(fragment.paintable()), fragment.index_in_node_for_point(offset_position_adjusted_by_scroll_offset), 0, 0 };
            if (callback(hit_test_result) == TraversalDecision::Break)
                return TraversalDecision::Break;
        } else if (type == HitTestType::TextCursor) {
            auto const* fragment_dom_node = fragment.layout_node().dom_node();
            if (common_ancestor_parent && fragment_dom_node && common_ancestor_parent->is_ancestor_of(*fragment_dom_node)) {
                // If we reached this point, the position is not within the fragment. However, the fragment start or end might be
                // the place to place the cursor. To determine the best place, we first find the closest fragment horizontally to
                // the cursor. If we could not find one, then find for the closest vertically above the cursor.
                // If we knew the direction of selection, we would look above if selecting upward.
                if (fragment_absolute_rect.bottom() - 1 <= offset_position_adjusted_by_scroll_offset.y()) { // fully below the fragment
                    HitTestResult hit_test_result {
                        .paintable = const_cast<Paintable&>(fragment.paintable()),
                        .index_in_node = fragment.start_offset() + fragment.length_in_code_units(),
                        .vertical_distance = offset_position_adjusted_by_scroll_offset.y() - fragment_absolute_rect.bottom(),
                    };
                    if (callback(hit_test_result) == TraversalDecision::Break)
                        return TraversalDecision::Break;
                } else if (fragment_absolute_rect.top() <= offset_position_adjusted_by_scroll_offset.y()) { // vertically within the fragment
                    if (offset_position_adjusted_by_scroll_offset.x() < fragment_absolute_rect.left()) {
                        HitTestResult hit_test_result {
                            .paintable = const_cast<Paintable&>(fragment.paintable()),
                            .index_in_node = fragment.start_offset(),
                            .vertical_distance = 0,
                            .horizontal_distance = fragment_absolute_rect.left() - offset_position_adjusted_by_scroll_offset.x(),
                        };
                        if (callback(hit_test_result) == TraversalDecision::Break)
                            return TraversalDecision::Break;
                    } else if (offset_position_adjusted_by_scroll_offset.x() > fragment_absolute_rect.right()) {
                        HitTestResult hit_test_result {
                            .paintable = const_cast<Paintable&>(fragment.paintable()),
                            .index_in_node = fragment.start_offset() + fragment.length_in_code_units(),
                            .vertical_distance = 0,
                            .horizontal_distance = offset_position_adjusted_by_scroll_offset.x() - fragment_absolute_rect.right(),
                        };
                        if (callback(hit_test_result) == TraversalDecision::Break)
                            return TraversalDecision::Break;
                    }
                }
            }
        }
    }

    if (!stacking_context() && is_visible() && (!layout_node().is_anonymous() || is_positioned())
        && absolute_border_box_rect().contains(offset_position_adjusted_by_scroll_offset)) {
        if (callback(HitTestResult { const_cast<PaintableWithLines&>(*this) }) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

void PaintableWithLines::resolve_paint_properties()
{
    Base::resolve_paint_properties();

    auto const& layout_node = this->layout_node();
    for (auto& fragment : fragments()) {
        if (!fragment.m_layout_node->is_text_node())
            continue;
        auto const& text_node = static_cast<Layout::TextNode const&>(*fragment.m_layout_node);

        auto const& font = fragment.m_layout_node->first_available_font();
        auto const glyph_height = CSSPixels::nearest_value_for(font.pixel_size());
        auto const css_line_thickness = [&] {
            auto const& thickness = text_node.computed_values().text_decoration_thickness();
            return thickness.value.visit(
                [glyph_height](CSS::TextDecorationThickness::Auto) {
                    // The UA chooses an appropriate thickness for text decoration lines; see below.
                    // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-auto
                    return max(glyph_height.scaled(0.1), 1);
                },
                [glyph_height](CSS::TextDecorationThickness::FromFont) {
                    // If the first available font has metrics indicating a preferred underline width, use that width,
                    // otherwise behaves as auto.
                    // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-from-font
                    // FIXME: Implement this properly.
                    return max(glyph_height.scaled(0.1), 1);
                },
                [&](CSS::LengthPercentage const& length_percentage) {
                    auto resolved_length = length_percentage.resolved(text_node, CSS::Length(1, CSS::LengthUnit::Em).to_px(text_node)).to_px(*fragment.m_layout_node);
                    return max(resolved_length, 1);
                });
        }();
        fragment.set_text_decoration_thickness(css_line_thickness);

        auto const& text_shadow = text_node.computed_values().text_shadow();
        Vector<ShadowData> resolved_shadow_data;
        if (!text_shadow.is_empty()) {
            resolved_shadow_data.ensure_capacity(text_shadow.size());
            for (auto const& layer : text_shadow) {
                resolved_shadow_data.empend(
                    layer.color,
                    layer.offset_x.to_px(layout_node),
                    layer.offset_y.to_px(layout_node),
                    layer.blur_radius.to_px(layout_node),
                    layer.spread_distance.to_px(layout_node),
                    ShadowPlacement::Outer);
            }
        }
        fragment.set_shadows(move(resolved_shadow_data));
    }
}

void PaintableWithLines::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    // Text shadows
    // This is yet another loop, but done here because all shadows should appear under all text.
    // So, we paint the shadows before painting any text.
    // FIXME: Find a smarter way to do this?

    for (auto const& fragment : fragments())
        paint_text_shadow(context, fragment, fragment.shadows());

    DOM::Position const* cursor_position = nullptr;
    if (auto const& navigable = document().navigable();
        navigable
        && navigable->is_focused()
        && document().cursor_blink_state()) {

        cursor_position = document().cursor_position().ptr();
    }

    DOM::Node const* cursor_node = cursor_position ? cursor_position->node().ptr() : nullptr;
    TextPaintable const* cursor_paintable = nullptr;

    for (auto const& fragment : m_fragments) {
        auto const* text_paintable = as_if<TextPaintable>(fragment.paintable());
        if (!text_paintable)
            continue;

        paint_text_fragment(context, *text_paintable, fragment);

        if (!cursor_paintable
            && cursor_node
            && text_paintable->dom_node() == cursor_node
            && text_paintable->selection_state() == Paintable::SelectionState::None)
            cursor_paintable = text_paintable;
    }

    if (cursor_paintable)
        paint_cursor_if_needed(context, *cursor_paintable, *cursor_position);
}

void PaintableWithLines::paint_cursor_if_needed(DisplayListRecordingContext& context, TextPaintable const& paintable, DOM::Position const& cursor_position) const
{
    auto const& document = paintable.document();

    auto cursor_offset = (size_t)cursor_position.offset();
    auto node_end = paintable.layout_node().text_for_rendering().length_in_code_units();

    PaintableFragment const* best_candidate = nullptr;

    for (auto const& candidate : m_fragments) {
        if (&candidate.paintable() != &paintable)
            continue;
        auto candidate_start = candidate.start_offset();
        if (candidate_start > cursor_offset)
            break;
        if (candidate.is_caret_anchor()) {
            if (cursor_offset != candidate_start && (cursor_offset != candidate_start + 1 || cursor_offset != node_end))
                continue;
        } else if (cursor_offset > candidate_start + candidate.length_in_code_units())
            continue;
        if (!best_candidate || best_candidate->is_caret_anchor())
            best_candidate = &candidate;
    }
    if (!best_candidate)
        return;

    auto const* active_element = document.active_element();
    auto active_element_is_editable = false;
    if (auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(active_element))
        active_element_is_editable = text_control->is_mutable();

    auto const* dom_node = best_candidate->layout_node().dom_node();
    if (!dom_node || (!dom_node->is_editable() && !active_element_is_editable))
        return;

    auto caret_color = paintable.computed_values().caret_color();
    if (caret_color.alpha() == 0 || paintable.selection_state() != Paintable::SelectionState::None)
        return;

    CSSPixelRect cursor_rect = best_candidate->cursor_rect(cursor_offset);
    if (cursor_rect.is_empty())
        return;

    auto cursor_device_rect = context.rounded_device_rect(cursor_rect).to_type<int>();

    context.display_list_recorder().fill_rect(cursor_device_rect, caret_color);
}

void paint_text_fragment(DisplayListRecordingContext& context, TextPaintable const& paintable, PaintableFragment const& fragment)
{
    if (!paintable.is_visible())
        return;

    auto& painter = context.display_list_recorder();

    auto fragment_absolute_rect = fragment.absolute_rect();
    auto fragment_enclosing_device_rect = context.enclosing_device_rect(fragment_absolute_rect).to_type<int>();

    if (context.should_show_line_box_borders())
        PaintableWithLines::paint_text_fragment_debug_highlight(context, fragment);

    auto glyph_run = fragment.glyph_run();
    if (!glyph_run)
        return;

    auto selection_rect = context.enclosing_device_rect(fragment.selection_rect()).to_type<int>();
    if (!selection_rect.is_empty())
        painter.fill_rect(selection_rect, CSS::SystemColor::highlight(paintable.computed_values().color_scheme()));

    auto scale = context.device_pixels_per_css_pixel();
    auto baseline_start = Gfx::FloatPoint {
        fragment_absolute_rect.x().to_float(),
        fragment_absolute_rect.y().to_float() + fragment.baseline().to_float(),
    } * scale;
    painter.draw_glyph_run(baseline_start, *glyph_run, paintable.computed_values().webkit_text_fill_color(), fragment_enclosing_device_rect, scale, fragment.orientation());

    paint_text_decoration(context, paintable, fragment);
}

void paint_text_decoration(DisplayListRecordingContext& context, TextPaintable const& paintable, PaintableFragment const& fragment)
{
    auto& recorder = context.display_list_recorder();
    auto& font = fragment.layout_node().first_available_font();
    auto fragment_box = fragment.absolute_rect();
    CSSPixels glyph_height = CSSPixels::nearest_value_for(font.pixel_size());
    auto baseline = fragment.baseline();

    auto line_color = paintable.computed_values().text_decoration_color();
    auto line_style = paintable.computed_values().text_decoration_style();
    auto device_line_thickness = context.rounded_device_pixels(fragment.text_decoration_thickness());
    auto text_decoration_lines = paintable.computed_values().text_decoration_line();
    auto text_underline_offset = paintable.computed_values().text_underline_offset();
    auto text_underline_position = paintable.computed_values().text_underline_position();
    for (auto line : text_decoration_lines) {
        DevicePixelPoint line_start_point {};
        DevicePixelPoint line_end_point {};

        if (line == CSS::TextDecorationLine::SpellingError) {
            // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-line-spelling-error
            // This value indicates the type of text decoration used by the user agent to highlight spelling mistakes.
            // Its appearance is UA-defined, and may be platform-dependent. It is often rendered as a red wavy underline.
            line_color = Color::Red;
            device_line_thickness = context.rounded_device_pixels(1);
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
            device_line_thickness = context.rounded_device_pixels(1);
            line_style = CSS::TextDecorationStyle::Wavy;
            line = CSS::TextDecorationLine::Underline;

            // https://drafts.csswg.org/css-text-decor-4/#underline-offset
            // When the value of the text-decoration-line property is either spelling-error or grammar-error, the UA
            // must ignore the value of text-underline-position.
            text_underline_offset = CSS::InitialValues::text_underline_offset();
        }

        switch (line) {
        case CSS::TextDecorationLine::None:
            return;
        case CSS::TextDecorationLine::Underline: {
            // https://drafts.csswg.org/css-text-decor-4/#text-underline-position-property
            auto underline_position_without_offset = [&]() {
                // FIXME: Support text-decoration: underline on vertical text
                switch (text_underline_position.horizontal) {
                case Web::CSS::TextUnderlinePositionHorizontal::Auto:
                    // The user agent may use any algorithm to determine the underline’s position; however it must be
                    // placed at or under the alphabetic baseline.

                    // Spec Note: It is suggested that the default underline position be close to the alphabetic
                    //            baseline,
                    // FIXME:     unless that would either cross subscripted (or otherwise lowered) text or draw over
                    //            glyphs from Asian scripts such as Han or Tibetan for which an alphabetic underline is
                    //            too high: in such cases, shifting the underline lower or aligning to the em box edge
                    //            as described for under may be more appropriate.
                    return fragment.baseline();
                case Web::CSS::TextUnderlinePositionHorizontal::FromFont:
                    // FIXME: If the first available font has metrics indicating a preferred underline offset, use that
                    //        offset, otherwise behaves as auto.
                    return fragment.baseline();
                case Web::CSS::TextUnderlinePositionHorizontal::Under:
                    // The underline is positioned under the element’s text content. In this case the underline usually
                    // does not cross the descenders. (This is sometimes called “accounting” underline.)
                    return fragment.baseline() + CSSPixels { font.pixel_metrics().descent };
                }

                VERIFY_NOT_REACHED();
            }();

            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, underline_position_without_offset + text_underline_offset));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(0, underline_position_without_offset + text_underline_offset));
            break;
        }
        case CSS::TextDecorationLine::Overline:
            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, baseline - glyph_height));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(0, baseline - glyph_height));
            break;
        case CSS::TextDecorationLine::LineThrough: {
            auto x_height = font.x_height();
            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, baseline - x_height * CSSPixels(0.5f)));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(0, baseline - x_height * CSSPixels(0.5f)));
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

        switch (line_style) {
        case CSS::TextDecorationStyle::Solid:
            recorder.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Solid);
            break;
        case CSS::TextDecorationStyle::Double:
            switch (line) {
            case CSS::TextDecorationLine::Underline:
                break;
            case CSS::TextDecorationLine::Overline:
                line_start_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                line_end_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                break;
            case CSS::TextDecorationLine::LineThrough:
                line_start_point.translate_by(0, -device_line_thickness / 2);
                line_end_point.translate_by(0, -device_line_thickness / 2);
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            recorder.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value());
            recorder.draw_line(line_start_point.translated(0, device_line_thickness + 1).to_type<int>(), line_end_point.translated(0, device_line_thickness + 1).to_type<int>(), line_color, device_line_thickness.value());
            break;
        case CSS::TextDecorationStyle::Dashed:
            recorder.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Dashed);
            break;
        case CSS::TextDecorationStyle::Dotted:
            recorder.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Dotted);
            break;
        case CSS::TextDecorationStyle::Wavy:
            auto amplitude = device_line_thickness.value() * 3;
            switch (line) {
            case CSS::TextDecorationLine::Underline:
                line_start_point.translate_by(0, device_line_thickness + context.rounded_device_pixels(1));
                line_end_point.translate_by(0, device_line_thickness + context.rounded_device_pixels(1));
                break;
            case CSS::TextDecorationLine::Overline:
                line_start_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                line_end_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                break;
            case CSS::TextDecorationLine::LineThrough:
                line_start_point.translate_by(0, -device_line_thickness / 2);
                line_end_point.translate_by(0, -device_line_thickness / 2);
                break;
            default:
                VERIFY_NOT_REACHED();
            }
            recorder.stroke_path({
                .cap_style = Gfx::Path::CapStyle::Round,
                .join_style = Gfx::Path::JoinStyle::Round,
                .miter_limit = 0,
                .dash_array = {},
                .dash_offset = 0,
                .path = build_triangle_wave_path(line_start_point.to_type<int>(), line_end_point.to_type<int>(), amplitude),
                .paint_style_or_color = line_color,
                .thickness = static_cast<float>(device_line_thickness.value()),
            });
            break;
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
