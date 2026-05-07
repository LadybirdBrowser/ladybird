/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/GraphemeEdgeTracker.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Painting {

template<typename Callback>
static void for_each_cluster_in_glyph_run(Gfx::GlyphRun const& glyph_run, size_t fragment_length_in_code_units, Callback&& callback)
{
    size_t cursor = 0;
    for (auto const& glyph : glyph_run.glyphs()) {
        if (glyph.glyph_width == 0)
            continue;

        auto cluster_start = cursor;
        auto cluster_end = min(cursor + glyph.length_in_code_units, fragment_length_in_code_units);
        cursor = cluster_end;

        if (cluster_end <= cluster_start)
            continue;

        if (callback(cluster_start, cluster_end, glyph.glyph_width) == IterationDecision::Break)
            break;
    }
}

PaintableFragment::PaintableFragment(Layout::LineBoxFragment const& fragment)
    : m_layout_node(fragment.layout_node())
    , m_offset(fragment.offset())
    , m_size(fragment.size())
    , m_start_offset(fragment.start())
    , m_length_in_code_units(fragment.length_in_code_units())
    , m_glyph_run(fragment.glyph_run())
    , m_baseline(fragment.baseline())
    , m_writing_mode(fragment.writing_mode())
    , m_has_trailing_whitespace(fragment.has_trailing_whitespace())
{
}

CSSPixelRect const PaintableFragment::absolute_rect() const
{
    CSSPixelRect rect { offset(), size() };
    if (auto containing_block = paintable().containing_block())
        rect.translate_by(containing_block->absolute_position());
    return rect;
}

size_t PaintableFragment::index_in_node_for_point(CSSPixelPoint position) const
{
    auto const* text_paintable = as_if<TextPaintable>(paintable());
    if (!text_paintable)
        return 0;

    auto relative_inline_offset = [&] {
        switch (orientation()) {
        case Orientation::Horizontal:
            return (position.x() - absolute_rect().x()).to_float();
        case Orientation::Vertical:
            return (position.y() - absolute_rect().y()).to_float();
        }
        VERIFY_NOT_REACHED();
    }();
    if (relative_inline_offset < 0)
        return 0;

    GraphemeEdgeTracker tracker { relative_inline_offset };

    if (m_glyph_run) {
        auto& segmenter = text_paintable->layout_node().grapheme_segmenter();

        // A single glyph can cover several code units / graphemes. In the case of ligatures, we want clicks inside to
        // snap to the closest grapheme boundary inside it, not jump to either end. To achieve this we walk
        // per-grapheme rather than per-glyph, splitting each glyph's advance proportionally across the graphemes that
        // lie inside its cluster.
        for_each_cluster_in_glyph_run(*m_glyph_run, m_length_in_code_units, [&](size_t cluster_start, size_t cluster_end, float cluster_width) {
            auto per_unit_advance = cluster_width / static_cast<float>(cluster_end - cluster_start);

            auto grapheme_start = m_start_offset + cluster_start;
            auto cluster_absolute_end = m_start_offset + cluster_end;
            while (grapheme_start < cluster_absolute_end) {
                auto grapheme_end = min(segmenter.next_boundary(grapheme_start).value_or(cluster_absolute_end), cluster_absolute_end);
                if (grapheme_end <= grapheme_start)
                    break;

                auto grapheme_units = grapheme_end - grapheme_start;
                auto grapheme_width = per_unit_advance * static_cast<float>(grapheme_units);
                if (tracker.update(grapheme_units, grapheme_width) == IterationDecision::Break)
                    return IterationDecision::Break;

                grapheme_start = grapheme_end;
            }

            return IterationDecision::Continue;
        });
    }

    return m_start_offset + tracker.resolve();
}

Optional<PaintableFragment::SelectionOffsets> PaintableFragment::compute_selection_offsets(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const
{
    auto const start_index = m_start_offset;
    auto const end_index = m_start_offset + m_length_in_code_units;

    switch (selection_state) {
    case Paintable::SelectionState::None:
        return {};
    case Paintable::SelectionState::Full:
        return SelectionOffsets { 0, m_length_in_code_units, true };
    case Paintable::SelectionState::StartAndEnd:
        if (start_index > end_offset_in_code_units || end_index < start_offset_in_code_units)
            return {};
        return SelectionOffsets {
            start_offset_in_code_units - min(start_offset_in_code_units, m_start_offset),
            min(end_offset_in_code_units - m_start_offset, m_length_in_code_units),
            end_offset_in_code_units >= end_index,
        };
    case Paintable::SelectionState::Start:
        if (end_index < start_offset_in_code_units)
            return {};
        return SelectionOffsets {
            start_offset_in_code_units - min(start_offset_in_code_units, m_start_offset),
            m_length_in_code_units,
            true,
        };
    case Paintable::SelectionState::End:
        if (start_index > end_offset_in_code_units)
            return {};
        return SelectionOffsets {
            0,
            min(end_offset_in_code_units - m_start_offset, m_length_in_code_units),
            end_offset_in_code_units >= end_index,
        };
    }
    VERIFY_NOT_REACHED();
}

CSSPixelRect PaintableFragment::range_rect(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const
{
    auto offsets = compute_selection_offsets(selection_state, start_offset_in_code_units, end_offset_in_code_units);
    if (!offsets.has_value())
        return {};

    auto rect = absolute_rect();
    auto const& font = glyph_run() ? glyph_run()->font() : layout_node().first_available_font();

    CSSPixels pixel_offset;
    CSSPixels pixel_width;

    // When entire fragment is selected, use the rect's existing dimensions rather than recalculating from text.
    if (offsets->start == 0 && offsets->end == m_length_in_code_units && m_length_in_code_units > 0) {
        pixel_offset = 0;
        pixel_width = rect.primary_size_for_orientation(orientation());
    } else {
        float offset_accumulator = 0.f;
        float width_accumulator = 0.f;

        if (m_glyph_run) {
            // Walk the glyph run, splitting any cluster that spans multiple code units proportionally between the
            // pre-selection offset and the in-selection width when the selection range partially overlaps the cluster.
            // This allows ligatures to be partially selected.
            for_each_cluster_in_glyph_run(*m_glyph_run, m_length_in_code_units, [&](size_t cluster_start, size_t cluster_end, float cluster_width) {
                if (cluster_end <= offsets->start) {
                    offset_accumulator += cluster_width;
                    return IterationDecision::Continue;
                }
                if (cluster_start >= offsets->end)
                    return IterationDecision::Continue;

                auto per_unit_advance = cluster_width / static_cast<float>(cluster_end - cluster_start);

                if (cluster_start < offsets->start)
                    offset_accumulator += per_unit_advance * static_cast<float>(offsets->start - cluster_start);

                auto in_sel_start = max(cluster_start, offsets->start);
                auto in_sel_end = min(cluster_end, offsets->end);
                width_accumulator += per_unit_advance * static_cast<float>(in_sel_end - in_sel_start);

                return IterationDecision::Continue;
            });
        }

        pixel_offset = CSSPixels { offset_accumulator };
        pixel_width = CSSPixels { width_accumulator };
    }

    // When start equals end, this is a cursor position.
    if (offsets->start == offsets->end)
        pixel_width = 1;

    // Include an additional space at the end if we remembered that this fragment contained trailing whitespace. This
    // shows the user that at least one whitespace character was present when selecting text, even though we don't store
    // that whitespace in the glyph run or text fragment.
    if (offsets->start != offsets->end && m_has_trailing_whitespace && offsets->include_trailing_whitespace)
        pixel_width += CSSPixels { font.glyph_width(' ') };

    rect.translate_primary_offset_for_orientation(orientation(), pixel_offset);
    rect.set_primary_size_for_orientation(orientation(), pixel_width);

    // Inflate so the rect covers glyph ascenders and descenders that may extend beyond the line box.
    auto const& font_metrics = font.pixel_metrics();
    if (font_metrics.ascent > 0.f || font_metrics.descent > 0.f) {
        CSSPixels ascent { font_metrics.ascent };
        CSSPixels descent { font_metrics.descent };
        auto overflow_top = max<CSSPixels>(0, ascent - m_baseline);
        auto overflow_bottom = max<CSSPixels>(0, descent - rect.secondary_size_for_orientation(orientation()) + m_baseline);
        rect.inflate_secondary_for_orientation(orientation(), overflow_top, overflow_bottom);
    }

    return rect;
}

Gfx::Orientation PaintableFragment::orientation() const
{
    switch (m_writing_mode) {
    case CSS::WritingMode::HorizontalTb:
        return Gfx::Orientation::Horizontal;
    case CSS::WritingMode::VerticalRl:
    case CSS::WritingMode::VerticalLr:
    case CSS::WritingMode::SidewaysRl:
    case CSS::WritingMode::SidewaysLr:
        return Gfx::Orientation::Vertical;
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<PaintableFragment::SelectionOffsets> PaintableFragment::selection_range_for_text_control() const
{
    // For focused text controls (input/textarea), determine selection from the control's internal state.
    auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(paintable().document().focused_area().ptr());
    if (!text_control)
        return {};
    if (paintable().dom_node() != text_control->form_associated_element_to_text_node())
        return {};

    auto selection_start = text_control->selection_start();
    auto selection_end = text_control->selection_end();
    if (selection_start == selection_end)
        return {};

    return SelectionOffsets { selection_start, selection_end };
}

Optional<PaintableFragment::SelectionOffsets> PaintableFragment::selection_offsets() const
{
    if (auto offsets = selection_range_for_text_control(); offsets.has_value())
        return compute_selection_offsets(Paintable::SelectionState::StartAndEnd, offsets->start, offsets->end);

    auto selection_state = paintable().selection_state();
    if (selection_state == Paintable::SelectionState::None)
        return {};

    auto selection = paintable().document().get_selection();
    if (!selection)
        return {};
    auto range = selection->range();
    if (!range)
        return {};

    return compute_selection_offsets(selection_state, range->start_offset(), range->end_offset());
}

CSSPixelRect PaintableFragment::selection_rect() const
{
    if (auto offsets = selection_range_for_text_control(); offsets.has_value())
        return range_rect(Paintable::SelectionState::StartAndEnd, offsets->start, offsets->end);

    auto const selection_state = paintable().selection_state();
    if (selection_state == Paintable::SelectionState::None)
        return {};

    auto selection = paintable().document().get_selection();
    if (!selection)
        return {};
    auto range = selection->range();
    if (!range)
        return {};

    return range_rect(selection_state, range->start_offset(), range->end_offset());
}

Utf16View PaintableFragment::text() const
{
    auto const* text_paintable = as_if<TextPaintable>(paintable());
    if (!text_paintable)
        return {};
    return text_paintable->layout_node().text_for_rendering().substring_view(m_start_offset, m_length_in_code_units);
}

}
