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
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>

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

PaintableFragment::PaintableFragment(PaintableWithLines const& paintable_with_lines, Layout::LineBoxFragment const& fragment, u32 line_index)
    : m_layout_node(fragment.layout_node())
    , m_paintable_with_lines(paintable_with_lines)
    , m_offset(fragment.offset())
    , m_size(fragment.size())
    , m_line_index(line_index)
    , m_start_offset(fragment.start())
    , m_length_in_code_units(fragment.length_in_code_units())
    , m_glyph_run(fragment.glyph_run())
    , m_baseline(fragment.baseline())
    , m_writing_mode(fragment.writing_mode())
{
    auto const* text_node = as_if<Layout::TextNode>(layout_node());
    if (text_node)
        m_dom_start_offset_in_node = text_node->dom_start_offset() + m_start_offset;
    else
        m_dom_start_offset_in_node = m_start_offset;

    if (fragment.has_trailing_whitespace() && text_node) {
        auto text = text_node->text_for_rendering().utf16_view();
        auto position = m_start_offset + m_length_in_code_units;
        while (position + m_trailing_whitespace_length_in_code_units < text.length_in_code_units()) {
            auto code_unit = text.code_unit_at(position + m_trailing_whitespace_length_in_code_units);
            if (code_unit != ' ' && code_unit != '\t')
                break;
            ++m_trailing_whitespace_length_in_code_units;
        }
    }
}

bool PaintableFragment::is_block_level_box() const
{
    auto const* layout_node_with_style = as_if<Layout::NodeWithStyle>(layout_node());
    return layout_node_with_style && layout_node_with_style->display().is_block_outside();
}

Layout::NodeWithStyle const& PaintableFragment::style_source() const
{
    if (auto const* node_with_style = as_if<Layout::NodeWithStyle>(layout_node()))
        return *node_with_style;
    return *layout_node().parent();
}

void PaintableFragment::set_selection_state(Paintable::SelectionState state)
{
    if (m_selection_state == state)
        return;

    m_selection_state = state;

    auto& paintable_with_lines = const_cast<PaintableWithLines&>(this->paintable_with_lines());
    paintable_with_lines.invalidate_paint_cache();
    for (auto ancestor = paintable_with_lines.parent(); ancestor; ancestor = ancestor->parent()) {
        if (auto* ancestor_box = ancestor.ptr())
            ancestor_box->invalidate_paint_cache();
    }

    if (auto const* self_painting_ancestor = nearest_self_painting_inline_box(layout_node()))
        self_painting_ancestor->invalidate_paint_cache();
}

CSSPixelRect const PaintableFragment::absolute_rect() const
{
    CSSPixelRect rect { offset(), size() };
    if (auto containing_block = containing_block_paintable())
        rect.translate_by(containing_block->absolute_position());
    return rect;
}

CSSPixelRect const PaintableFragment::absolute_line_box_rect() const
{
    auto const& lines = paintable_with_lines().lines();
    if (m_line_index >= lines.size())
        return {};
    auto rect = lines[m_line_index].rect;
    if (auto containing_block = containing_block_paintable())
        rect.translate_by(containing_block->absolute_position());
    return rect;
}

RefPtr<Paintable> PaintableFragment::containing_block_paintable() const
{
    if (auto paintable = layout_node().paintable())
        return paintable->containing_block();

    auto const* containing_block = layout_node().containing_block();
    if (!containing_block)
        return nullptr;
    return const_cast<Layout::Box&>(*containing_block).paintable_box();
}

PaintableFragment::CaretMatch PaintableFragment::caret_match(size_t offset, TextAffinity affinity) const
{
    if (offset < dom_start_offset_in_node() || offset > dom_end_offset_with_trailing_whitespace())
        return CaretMatch::None;
    // At a soft wrap boundary, Downstream affinity prefers a later fragment starting exactly at the offset; this
    // fragment only applies if none exists.
    if (affinity == TextAffinity::Downstream && offset == dom_end_offset_with_trailing_whitespace())
        return CaretMatch::SoftWrapFallback;
    return CaretMatch::Direct;
}

size_t PaintableFragment::index_in_node_for_point(CSSPixelPoint position) const
{
    auto const* text_node = as_if<Layout::TextNode>(layout_node());
    if (!text_node)
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
    auto reached_target = false;

    if (m_glyph_run) {
        auto& segmenter = text_node->grapheme_segmenter();

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
                if (tracker.update(grapheme_units, grapheme_width) == IterationDecision::Break) {
                    reached_target = true;
                    return IterationDecision::Break;
                }

                grapheme_start = grapheme_end;
            }

            return IterationDecision::Continue;
        });
    }

    // Points past the fragment's text can still land inside whitespace hanging at a soft wrap point.
    if (!reached_target) {
        if (auto trailing_whitespace_length = trailing_whitespace_length_in_code_units(); trailing_whitespace_length > 0) {
            auto const& font = m_glyph_run ? m_glyph_run->font() : layout_node().first_available_font();
            auto space_width = font.glyph_width(' ');
            for (size_t i = 0; i < trailing_whitespace_length; ++i) {
                if (tracker.update(1, space_width) == IterationDecision::Break)
                    break;
            }
        }
    }

    return dom_start_offset_in_node() + tracker.resolve();
}

Optional<PaintableFragment::SelectionOffsets> PaintableFragment::compute_selection_offsets(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const
{
    // Whitespace hanging at a soft wrap point belongs to this fragment's line, so it is part of this fragment's
    // selectable range even though it is not part of the fragment's text.
    auto const length_with_trailing_whitespace = m_length_in_code_units + trailing_whitespace_length_in_code_units();
    auto const dom_start = dom_start_offset_in_node();
    auto const dom_end = dom_start + length_with_trailing_whitespace;

    switch (selection_state) {
    case Paintable::SelectionState::None:
        return {};
    case Paintable::SelectionState::Full:
        return SelectionOffsets { 0, length_with_trailing_whitespace };
    case Paintable::SelectionState::StartAndEnd:
        if (dom_start > end_offset_in_code_units || dom_end < start_offset_in_code_units)
            return {};
        return SelectionOffsets {
            start_offset_in_code_units - min(start_offset_in_code_units, dom_start),
            min(end_offset_in_code_units - dom_start, length_with_trailing_whitespace),
        };
    case Paintable::SelectionState::Start:
        if (dom_end < start_offset_in_code_units)
            return {};
        return SelectionOffsets {
            start_offset_in_code_units - min(start_offset_in_code_units, dom_start),
            length_with_trailing_whitespace,
        };
    case Paintable::SelectionState::End:
        if (dom_start > end_offset_in_code_units)
            return {};
        return SelectionOffsets {
            0,
            min(end_offset_in_code_units - dom_start, length_with_trailing_whitespace),
        };
    }
    VERIFY_NOT_REACHED();
}

CSSPixelRect PaintableFragment::range_rect(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const
{
    auto offsets = compute_selection_offsets(selection_state, start_offset_in_code_units, end_offset_in_code_units);
    if (!offsets.has_value())
        return {};
    return rect_for_selection_offsets(*offsets);
}

CSSPixelRect PaintableFragment::rect_for_selection_offsets(SelectionOffsets const& offsets) const
{
    auto rect = absolute_rect();
    auto const& font = glyph_run() ? glyph_run()->font() : layout_node().first_available_font();

    // Portions of the range beyond the fragment's text cover whitespace hanging at a soft wrap point.
    auto const start_in_text = min(offsets.start, m_length_in_code_units);
    auto const end_in_text = min(offsets.end, m_length_in_code_units);

    CSSPixels pixel_offset;
    CSSPixels pixel_width;

    // When entire fragment is selected, use the rect's existing dimensions rather than recalculating from text.
    if (start_in_text == 0 && end_in_text == m_length_in_code_units && m_length_in_code_units > 0) {
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
                if (cluster_end <= start_in_text) {
                    offset_accumulator += cluster_width;
                    return IterationDecision::Continue;
                }
                if (cluster_start >= end_in_text)
                    return IterationDecision::Continue;

                auto per_unit_advance = cluster_width / static_cast<float>(cluster_end - cluster_start);

                if (cluster_start < start_in_text)
                    offset_accumulator += per_unit_advance * static_cast<float>(start_in_text - cluster_start);

                auto in_sel_start = max(cluster_start, start_in_text);
                auto in_sel_end = min(cluster_end, end_in_text);
                width_accumulator += per_unit_advance * static_cast<float>(in_sel_end - in_sel_start);

                return IterationDecision::Continue;
            });
        }

        pixel_offset = CSSPixels { offset_accumulator };
        pixel_width = CSSPixels { width_accumulator };
    }

    // Hanging whitespace is not part of the glyph run; account for it with space glyph widths.
    if (offsets.start > m_length_in_code_units || offsets.end > m_length_in_code_units) {
        CSSPixels space_width { font.glyph_width(' ') };
        auto trailing_units_before_start = offsets.start > m_length_in_code_units ? offsets.start - m_length_in_code_units : 0;
        auto trailing_units_before_end = offsets.end > m_length_in_code_units ? offsets.end - m_length_in_code_units : 0;
        pixel_offset += space_width * static_cast<int>(trailing_units_before_start);
        pixel_width += space_width * static_cast<int>(trailing_units_before_end - trailing_units_before_start);
    }

    // When start equals end, this is a cursor position.
    if (offsets.start == offsets.end)
        pixel_width = 1;

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
    auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(layout_node().document().focused_area().ptr());
    if (!text_control)
        return {};
    if (layout_node().dom_node() != text_control->form_associated_element_to_text_node())
        return {};

    auto selection_start = text_control->selection_start();
    auto selection_end = text_control->selection_end();
    if (selection_start == selection_end)
        return {};

    return SelectionOffsets { selection_start, selection_end };
}

Optional<PaintableFragment::SelectionOffsets> PaintableFragment::selection_offsets() const
{
    auto drop_degenerate_offsets = [](Optional<SelectionOffsets> offsets) -> Optional<SelectionOffsets> {
        // A selection that only touches this fragment's boundary selects nothing within it.
        if (offsets.has_value() && offsets->start == offsets->end)
            return {};
        return offsets;
    };

    if (auto offsets = selection_range_for_text_control(); offsets.has_value())
        return drop_degenerate_offsets(compute_selection_offsets(Paintable::SelectionState::StartAndEnd, offsets->start, offsets->end));

    if (m_selection_state == Paintable::SelectionState::None)
        return {};

    auto selection = layout_node().document().get_selection();
    if (!selection)
        return {};
    auto range = selection->range();
    if (!range)
        return {};

    return drop_degenerate_offsets(compute_selection_offsets(m_selection_state, range->start_offset(), range->end_offset()));
}

CSSPixelRect PaintableFragment::selection_rect() const
{
    if (auto offsets = selection_offsets(); offsets.has_value())
        return rect_for_selection_offsets(*offsets);
    return {};
}

Utf16View PaintableFragment::text() const
{
    auto const* text_node = as_if<Layout::TextNode>(layout_node());
    if (!text_node)
        return {};
    return text_node->text_for_rendering().substring_view(m_start_offset, m_length_in_code_units);
}

}
