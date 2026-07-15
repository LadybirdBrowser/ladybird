/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/ShadowPainting.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/VisualLines.h>

namespace Web::Painting {

static void paint_text_decoration(DisplayListRecordingContext&, Layout::TextNode const&, PaintableFragment::FragmentSpan const&);
static Gfx::Path build_triangle_wave_path(Gfx::IntPoint from, Gfx::IntPoint to, float amplitude);
static void compute_render_spans(PaintableFragment const&, Vector<PaintableFragment::FragmentSpan, 4>&);
static void paint_text_fragment(DisplayListRecordingContext&, PaintableFragment::FragmentSpan const&);

static bool layout_node_is_visible(Layout::NodeWithStyle const& layout_node)
{
    auto const& computed_values = layout_node.computed_values();
    return computed_values.visibility() == CSS::Visibility::Visible && computed_values.opacity() != 0;
}

NonnullRefPtr<PaintableWithLines> PaintableWithLines::create(Layout::BlockContainer const& block_container)
{
    return adopt_ref(*new PaintableWithLines(block_container));
}

PaintableWithLines::PaintableWithLines(Layout::BlockContainer const& layout_box)
    : Paintable(layout_box)
{
}

PaintableWithLines::~PaintableWithLines()
{
}

void PaintableWithLines::reset_for_relayout()
{
    Paintable::reset_for_relayout();
    m_fragments.clear();
    m_lines.clear();
    m_inline_box_pieces.clear();
    m_fragment_ownership_filter = {
        .include_everything = true,
        .included = {},
        .excluded = {},
    };
    m_text_fragment_properties_paint_generation_id.clear();
}

InlinePaintable const* nearest_self_painting_inline_box(Layout::Node const& node)
{
    for (auto const* ancestor = node.nearest_fragmented_inline_ancestor(); ancestor; ancestor = ancestor->nearest_fragmented_inline_ancestor()) {
        auto const* proxy = as_if<InlinePaintable>(ancestor->paintable().ptr());
        if (proxy && proxy->is_self_painting())
            return proxy;
    }
    return nullptr;
}

void PaintableWithLines::assign_inline_box_geometry()
{
    HashMap<Layout::Node const*, Vector<u32>> piece_indices_by_node;
    for (u32 piece_index = 0; piece_index < m_inline_box_pieces.size(); ++piece_index) {
        if (auto const* piece_node = m_inline_box_pieces[piece_index].node.ptr())
            piece_indices_by_node.ensure(piece_node).append(piece_index);
    }

    for (auto& [piece_node, piece_indices] : piece_indices_by_node) {
        auto* inline_paintable = as_if<InlinePaintable>(const_cast<Layout::Node*>(piece_node)->paintable().ptr());
        if (!inline_paintable)
            continue;

        auto const& box_model = inline_paintable->box_model();

        Optional<CSSPixelRect> content_union;
        Optional<CSSPixelRect> padding_union;
        Optional<CSSPixelRect> border_union;
        auto unite = [](Optional<CSSPixelRect>& target, CSSPixelRect const& rect) {
            if (!target.has_value()) {
                target = rect;
                return;
            }
            // Degenerate rects (from placeholder pieces) only establish a position; the
            // first one wins, and any real rect takes precedence over them.
            if (rect.is_empty())
                return;
            if (target->is_empty())
                target = rect;
            else
                target->unite(rect);
        };

        for (auto piece_index : piece_indices) {
            auto const& piece = m_inline_box_pieces[piece_index];
            if (piece.is_geometry_only_placeholder) {
                // Placeholder pieces carry the (degenerate) content rect directly.
                auto content_rect = piece.border_box_rect;
                auto padding_rect = content_rect.inflated(box_model.padding.top, box_model.padding.right, box_model.padding.bottom, box_model.padding.left);
                auto border_rect = padding_rect.inflated(box_model.border.top, box_model.border.right, box_model.border.bottom, box_model.border.left);
                unite(content_union, content_rect);
                unite(padding_union, padding_rect);
                unite(border_union, border_rect);
                continue;
            }
            auto border_rect = piece.border_box_rect;
            auto padding_rect = inline_paintable->piece_padding_box_rect(piece, border_rect);
            auto content_rect = inline_paintable->piece_content_box_rect(piece, border_rect);
            unite(content_union, content_rect);
            unite(padding_union, padding_rect);
            unite(border_union, border_rect);
        }

        if (!content_union.has_value())
            continue;
        inline_paintable->set_offset(content_union->location());
        inline_paintable->set_content_size(content_union->size());
        inline_paintable->set_local_box_unions(
            padding_union->translated(-content_union->x(), -content_union->y()),
            border_union->translated(-content_union->x(), -content_union->y()));
        inline_paintable->set_piece_indices(move(piece_indices));
    }
}

void PaintableWithLines::assign_fragment_ownership()
{
    m_fragment_ownership_filter = {
        .include_everything = true,
        .included = {},
        .excluded = {},
    };

    // A box that stopped being self-painting since the last assignment must not keep its
    // stale filter around, so start every piece's box from a clean slate.
    for (auto const& piece : m_inline_box_pieces) {
        if (auto const* piece_node = piece.node.ptr()) {
            if (auto* piece_paintable = as_if<InlinePaintable>(const_cast<Layout::Node*>(piece_node)->paintable().ptr()))
                piece_paintable->set_fragment_ownership_filter({});
        }
    }

    HashMap<InlinePaintable*, FragmentOwnershipFilter> filters;
    for (auto const& piece : m_inline_box_pieces) {
        auto const* piece_node = piece.node.ptr();
        if (!piece_node || piece.fragment_count == 0)
            continue;
        auto* piece_paintable = as_if<InlinePaintable>(const_cast<Layout::Node*>(piece_node)->paintable().ptr());
        if (!piece_paintable || !piece_paintable->is_self_painting())
            continue;
        FragmentRange range { piece.first_fragment_index, piece.first_fragment_index + piece.fragment_count };
        filters.ensure(piece_paintable).included.append(range);
        // The nearest self-painting box above owns the surrounding content but must not
        // paint this box's subtree; content outside any such box falls to the block.
        if (auto const* enclosing_owner = nearest_self_painting_inline_box(*piece_node))
            filters.ensure(const_cast<InlinePaintable*>(enclosing_owner)).excluded.append(range);
        else
            m_fragment_ownership_filter.excluded.append(range);
    }

    auto sort_ranges = [](Vector<FragmentRange, 4>& ranges) {
        quick_sort(ranges, [](auto const& a, auto const& b) { return a.begin < b.begin; });
    };
    sort_ranges(m_fragment_ownership_filter.excluded);
    for (auto& [piece_paintable, filter] : filters) {
        sort_ranges(filter.excluded);
        piece_paintable->set_fragment_ownership_filter(move(filter));
    }
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

void PaintableWithLines::record_hit_test_items(DisplayListRecordingContext& context, PaintPhase phase) const
{
    Paintable::record_hit_test_items(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    auto* hit_test_display_list = context.hit_test_display_list();
    if (!hit_test_display_list)
        return;

    if (m_fragments.is_empty() && !has_children()) {
        if (is_visible() && visible_for_hit_testing())
            record_empty_editable_hit_test_item(*hit_test_display_list);
        return;
    }

    // Fragments inside self-painting inline boxes are recorded during that box's paint,
    // so their hit-test order and visual context match where they are painted.
    m_fragment_ownership_filter.for_each_owned_fragment_index(m_fragments.size(), [&](size_t index) {
        auto const& fragment = m_fragments[index];
        if (fragment.is_block_level_box())
            return;
        hit_test_display_list->append_text_fragment(fragment, accumulated_visual_context_for_descendants_index());
    });

    record_empty_line_caret_items(*hit_test_display_list, accumulated_visual_context_for_descendants_index());
}

Vector<PaintableWithLines::EmptyLineCaretTarget> PaintableWithLines::empty_line_caret_targets() const
{
    if (m_fragments.is_empty() || m_lines.is_empty())
        return {};

    // Line boxes without fragments (e.g. the blank line between two consecutive newlines in a textarea) produce no
    // fragments to hit test or paint a caret in. When all fragments belong to a single text node with preserved
    // newlines, we can derive the caret offset of each empty line and compute caret targets for them.
    auto const* text_layout_node = as_if<Layout::TextNode>(m_fragments.first().layout_node());
    if (!text_layout_node)
        return {};
    if (!white_space_preserves_newlines(*text_layout_node))
        return {};

    // FIXME: Support vertical writing modes.
    if (computed_values().writing_mode() != CSS::WritingMode::HorizontalTb)
        return {};

    auto const* dom_text = text_layout_node->dom_text();
    if (!dom_text)
        return {};
    if (!text_contains_empty_visual_line_positions(dom_text->data().utf16_view()))
        return {};

    for (auto const& fragment : m_fragments) {
        if (&fragment.layout_node() != text_layout_node)
            return {};
    }

    auto lines = collect_visual_lines(*dom_text);

    // The mapping below requires visual lines to correspond 1:1 to this block's line boxes. Trailing blank lines are
    // the exception: layout does not retain a line box for a blank line at the very end, so visual lines may extend
    // past m_lines and get extrapolated rects below the last line box.
    if (lines.size() < m_lines.size())
        return {};
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i >= m_lines.size()) {
            if (!lines[i].fragments.is_empty())
                return {};
            continue;
        }
        if (lines[i].fragments.is_empty() != (m_lines[i].fragment_count == 0))
            return {};
        if (!lines[i].fragments.is_empty() && lines[i].fragments.first()->line_index() != i)
            return {};
    }

    Vector<EmptyLineCaretTarget> targets;
    auto content_rect = absolute_rect();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].fragments.is_empty())
            continue;

        CSSPixelRect line_rect;
        if (i < m_lines.size()) {
            line_rect = m_lines[i].rect.translated(content_rect.location());
        } else {
            auto last_rect = m_lines.last().rect.translated(content_rect.location());
            auto steps = static_cast<int>(i - (m_lines.size() - 1));
            line_rect = { content_rect.x(), last_rect.bottom() + last_rect.height() * (steps - 1), content_rect.width(), last_rect.height() };
        }

        targets.append({ lines[i].start_offset, i, line_rect });
    }
    return targets;
}

// A cursor on a line box with no fragments (e.g. a blank line in a textarea) has no fragment to position itself in;
// it is placed at the start of the empty line.
Optional<CSSPixelRect> PaintableWithLines::empty_line_caret_rect(DOM::Position const& position) const
{
    if (m_fragments.is_empty())
        return {};
    auto const* text_layout_node = as_if<Layout::TextNode>(m_fragments.first().layout_node());
    if (!text_layout_node || position.node() != text_layout_node->dom_text())
        return {};
    for (auto const& target : empty_line_caret_targets()) {
        if (target.offset == position.offset())
            return target.rect;
    }
    return {};
}

void PaintableWithLines::record_empty_line_caret_items(HitTestDisplayList& hit_test_display_list, VisualContextIndex visual_context_index) const
{
    for (auto const& target : empty_line_caret_targets())
        hit_test_display_list.append_empty_line(m_fragments.first(), target.offset, target.line_index, target.rect, visual_context_index);
}

static void resolve_text_fragment_properties(PaintableWithLines const& paintable_with_lines)
{
    for (auto& fragment : const_cast<PaintableWithLines&>(paintable_with_lines).fragments()) {
        auto const* text_node = as_if<Layout::TextNode>(fragment.layout_node());
        if (!text_node)
            continue;

        auto const& font = text_node->first_available_font();
        auto const glyph_height = CSSPixels::nearest_value_for(font.pixel_size());
        auto const line_thickness = [&] {
            auto const& thickness = text_node->parent()->computed_values().text_decoration_thickness();
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
                    auto resolved_length = length_percentage.resolved(CSS::Length(1, CSS::LengthUnit::Em).to_px(*text_node->parent())).to_px(*text_node->parent());
                    return max(resolved_length, 1);
                });
        }();
        fragment.set_text_decoration_thickness(line_thickness);

        auto const& text_shadow = text_node->parent()->computed_values().text_shadow();
        Vector<ShadowData> resolved_shadow_data;
        if (!text_shadow.is_empty()) {
            resolved_shadow_data.ensure_capacity(text_shadow.size());
            for (auto const& layer : text_shadow)
                resolved_shadow_data.append(ShadowData::from_css(layer));
        }
        fragment.set_shadows(move(resolved_shadow_data));
    }
}

void PaintableWithLines::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (is_visible())
        Paintable::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        // visibility: hidden on this block does not hide descendants that set visibility:
        // visible again, so fragments (and the caret between their glyphs) are filtered by
        // their own node's visibility instead.
        paint_fragments_foreground(context, m_fragment_ownership_filter);

        if (document().cursor_position())
            paint_cursor(context, nullptr);
    }
}

void PaintableWithLines::paint_fragments_foreground(DisplayListRecordingContext& context, FragmentOwnershipFilter const& filter) const
{
    // The resolved properties are shared by all owners painting fragments of this block,
    // so resolving once per display list build is enough.
    if (m_text_fragment_properties_paint_generation_id != context.paint_generation_id()) {
        resolve_text_fragment_properties(*this);
        m_text_fragment_properties_paint_generation_id = context.paint_generation_id();
    }

    Vector<PaintableFragment::FragmentSpan, 4> spans;
    filter.for_each_owned_fragment_index(m_fragments.size(), [&](size_t index) {
        compute_render_spans(m_fragments[index], spans);
    });

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
}

void compute_render_spans(PaintableFragment const& fragment, Vector<PaintableFragment::FragmentSpan, 4>& spans)
{
    if (fragment.is_block_level_box())
        return;

    auto const* text_node = as_if<Layout::TextNode>(fragment.layout_node());
    if (!text_node) {
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

    if (!layout_node_is_visible(*text_node->parent()))
        return;

    auto text_color = text_node->parent()->computed_values().webkit_text_fill_color();
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

    auto [selection_start, selection_end] = *selection_offsets;
    auto selection_style = Paintable::selection_style_for_node(*text_node, text_node->dom_text());
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

    auto const& text_node = as<Layout::TextNode>(fragment.layout_node());

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
            fragment.dom_start_offset_in_node() + span.start_code_unit,
            fragment.dom_start_offset_in_node() + span.end_code_unit);
        auto span_rect = context.rounded_device_rect(range_rect).to_type<int>();
        painter.save();
        painter.add_clip_rect(span_rect);
        painter.draw_glyph_run(baseline_start, *glyph_run, span.text_color, fragment_device_rect, scale, fragment.orientation());
        painter.restore();
    }

    paint_text_decoration(context, text_node, span);
}

Optional<PaintableFragment const&> PaintableWithLines::fragment_at_position(DOM::Position const& position) const
{
    PaintableFragment const* fallback_fragment = nullptr;
    for (auto const& fragment : m_fragments) {
        auto const* text_node = as_if<Layout::TextNode>(fragment.layout_node());
        if (!text_node || position.node() != text_node->dom_text())
            continue;
        switch (fragment.caret_match(position.offset(), position.affinity())) {
        case PaintableFragment::CaretMatch::None:
            continue;
        case PaintableFragment::CaretMatch::SoftWrapFallback:
            if (!fallback_fragment)
                fallback_fragment = &fragment;
            continue;
        case PaintableFragment::CaretMatch::Direct:
            return fragment;
        }
    }
    if (fallback_fragment)
        return *fallback_fragment;
    return {};
}

CSSPixelRect PaintableWithLines::caret_rect_for_child_offset(size_t offset) const
{
    auto content_box = absolute_padding_box_rect();
    auto line_height = computed_values().line_height();
    CSSPixelRect rect { content_box.x(), content_box.y(), 1, line_height };

    auto dom_node = layout_node().dom_node();
    if (!dom_node)
        return rect;
    auto* child = dom_node->child_at_index(offset);
    if (!child || !is<HTML::HTMLBRElement>(*child))
        return rect;

    // A caret parked before a <br> sits on the line below the content preceding the <br>. Layout produces no
    // fragments for <br>, so start below the fragments of any preceding content, and add one line height for each
    // empty line rendered by earlier <br>s.
    Optional<CSSPixels> preceding_content_bottom;
    for_each_in_inclusive_subtree_of_type<PaintableWithLines>([&](auto const& paintable_with_lines) {
        for (auto const& fragment : paintable_with_lines.fragments()) {
            auto* fragment_dom_node = const_cast<DOM::Node*>(fragment.layout_node().dom_node());
            if (!fragment_dom_node || !(const_cast<DOM::Node&>(*child).compare_document_position(fragment_dom_node) & DOM::Node::DOCUMENT_POSITION_PRECEDING))
                continue;
            auto bottom = fragment.absolute_rect().bottom();
            if (!preceding_content_bottom.has_value() || bottom > *preceding_content_bottom)
                preceding_content_bottom = bottom;
        }
        return TraversalDecision::Continue;
    });

    size_t preceding_empty_lines = 0;
    dom_node->for_each_in_subtree_of_type<HTML::HTMLBRElement>([&](auto& br) {
        if (&br == child)
            return TraversalDecision::Break;
        if (br.represents_empty_line())
            ++preceding_empty_lines;
        return TraversalDecision::Continue;
    });

    rect.set_y(preceding_content_bottom.value_or(content_box.y()) + line_height * preceding_empty_lines);
    return rect;
}

void PaintableWithLines::paint_cursor(DisplayListRecordingContext& context, InlinePaintable const* owner) const
{
    if (!should_paint_cursor())
        return;

    auto cursor_position = document().cursor_position();
    VERIFY(cursor_position);

    auto const* dom_node = layout_node().dom_node();

    auto fragment = fragment_at_position(*cursor_position);

    CSSPixelRect cursor_rect;
    Color caret_color;

    if (fragment.has_value()) {
        // The caret paints where its fragment's foreground paints: inside the nearest
        // self-painting inline box, or in the block itself.
        if (nearest_self_painting_inline_box(fragment->layout_node()) != owner)
            return;
        // Like the glyphs around it, the caret follows the text's own visibility, which may
        // differ from this box's.
        if (!layout_node_is_visible(fragment->style_source()))
            return;
        caret_color = fragment->style_source().computed_values().caret_color();
        cursor_rect = fragment->range_rect(SelectionState::StartAndEnd, cursor_position->offset(), cursor_position->offset());
    } else if (owner) {
        // Blank lines and empty editable elements are handled by the block / the box itself.
        return;
    } else if (!is_visible()) {
        // Blank-line and empty-element carets belong to this block itself.
        return;
    } else if (auto empty_line_rect = empty_line_caret_rect(*cursor_position); empty_line_rect.has_value()) {
        caret_color = m_fragments.first().style_source().computed_values().caret_color();
        cursor_rect = { empty_line_rect->x(), empty_line_rect->y(), 1, empty_line_rect->height() };
    } else {
        // Empty editable elements have no fragments, but should still draw a cursor.
        if (cursor_position->node() != dom_node)
            return;

        caret_color = computed_values().caret_color();
        cursor_rect = caret_rect_for_child_offset(cursor_position->offset());
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

void paint_text_decoration(DisplayListRecordingContext& context, Layout::TextNode const& text_node, PaintableFragment::FragmentSpan const& span)
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
        line_color = text_node.parent()->computed_values().text_decoration_color();
        line_style = text_node.parent()->computed_values().text_decoration_style();
        text_decoration_lines = text_node.parent()->computed_values().text_decoration_line();
    }

    // Compute the decoration box for this span.
    auto fragment_box = fragment.absolute_rect();
    if (span.start_code_unit != 0 || span.end_code_unit != fragment.length_in_code_units()) {
        auto span_rect = fragment.range_rect(Paintable::SelectionState::StartAndEnd,
            fragment.dom_start_offset_in_node() + span.start_code_unit,
            fragment.dom_start_offset_in_node() + span.end_code_unit);
        fragment_box.set_x(span_rect.x());
        fragment_box.set_width(span_rect.width());
    }
    auto text_underline_offset = text_node.parent()->computed_values().text_underline_offset();
    auto text_underline_position = text_node.parent()->computed_values().text_underline_position();
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
        auto skip_ink = text_node.parent()->computed_values().text_decoration_skip_ink();
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
