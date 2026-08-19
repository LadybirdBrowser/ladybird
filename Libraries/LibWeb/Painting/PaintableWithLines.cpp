/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
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
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/VisualLines.h>

namespace Web::Painting {

static void compute_render_spans(PaintableFragment const&, Vector<PaintableFragment::FragmentSpan, 4>&);

static bool layout_node_is_visible(Layout::NodeWithStyle const& layout_node)
{
    return layout_node.visibility() == CSS::Visibility::Visible && layout_node.opacity() != 0;
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
    if (layout_node().writing_mode() != CSS::WritingMode::HorizontalTb)
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
    if (!text_layout_node || position.node() != GC::Ptr { text_layout_node->dom_text() })
        return {};
    for (auto const& target : empty_line_caret_targets()) {
        if (target.offset == position.offset())
            return target.rect;
    }
    return {};
}

void PaintableWithLines::for_each_empty_line_caret_item(Function<void(EmptyLineCaretItem const&)> const& callback) const
{
    for (auto const& target : empty_line_caret_targets())
        callback({ .is_line_break_boundary = false, .caret_offset = target.offset, .line_index = target.line_index, .rect = target.rect });

    auto* dom_node = layout_node().dom_node();
    if (!dom_node || m_fragments.is_empty())
        return;
    // A <br> between fragment-backed lines does not produce a fragment of its own, so record its parent boundary as
    // a caret target. This covers leading and consecutive editable line breaks.
    for (auto* child = dom_node->first_child(); child; child = child->next_sibling()) {
        auto* br = as_if<HTML::HTMLBRElement>(*child);
        if (!br || !br->represents_empty_line())
            continue;
        callback({ .is_line_break_boundary = true, .caret_offset = br->index(), .line_index = 0, .rect = caret_rect_for_child_offset(br->index()) });
    }
}

static void resolve_text_fragment_properties(PaintableWithLines const& paintable_with_lines)
{
    for (auto& fragment : const_cast<PaintableWithLines&>(paintable_with_lines).fragments()) {
        auto const* text_node = as_if<Layout::TextNode>(fragment.layout_node());
        if (!text_node)
            continue;

        auto const& text_shadow = text_node->parent()->text_shadow();
        Vector<ShadowData> resolved_shadow_data;
        if (!text_shadow.is_empty()) {
            resolved_shadow_data.ensure_capacity(text_shadow.size());
            for (auto const& layer : text_shadow)
                resolved_shadow_data.append(ShadowData::from_css(layer));
        }
        fragment.set_shadows(move(resolved_shadow_data));
    }
}

Vector<PaintableFragment::FragmentSpan, 4> PaintableWithLines::render_spans_for_paint(u64 paint_generation_id, ReadonlySpan<u32> owned_fragment_indices) const
{
    // The resolved properties are shared by all owners painting fragments of this block,
    // so resolving once per display list build is enough.
    if (m_text_fragment_properties_paint_generation_id != paint_generation_id) {
        resolve_text_fragment_properties(*this);
        m_text_fragment_properties_paint_generation_id = paint_generation_id;
    }

    Vector<PaintableFragment::FragmentSpan, 4> spans;
    for (auto index : owned_fragment_indices)
        compute_render_spans(m_fragments[index], spans);
    return spans;
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

    auto text_color = text_node->parent()->webkit_text_fill_color();
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

Optional<PaintableFragment const&> PaintableWithLines::fragment_at_position(DOM::Position const& position) const
{
    PaintableFragment const* fallback_fragment = nullptr;
    for (auto const& fragment : m_fragments) {
        auto const* text_node = as_if<Layout::TextNode>(fragment.layout_node());
        if (!text_node || position.node() != GC::Ptr { text_node->dom_text() })
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
    auto line_height = layout_node().line_height();
    CSSPixelRect rect { content_box.x(), content_box.y(), 1, line_height };

    auto dom_node = layout_node().dom_node();
    if (!dom_node)
        return rect;

    // A boundary immediately after an atomic inline element paints after that element. Atomic inline elements have
    // no text offset for fragment_at_position() to resolve, so use their inline edge directly.
    if (offset > 0) {
        auto* previous_child = dom_node->child_at_index(offset - 1);
        if (previous_child) {
            for (auto const& fragment : m_fragments) {
                auto* fragment_dom_node = fragment.layout_node().dom_node();
                if (fragment_dom_node != previous_child || !fragment.layout_node().is_atomic_inline())
                    continue;

                auto fragment_rect = fragment.absolute_rect();
                if (layout_node().writing_mode() == CSS::WritingMode::HorizontalTb)
                    rect.set_x(layout_node().inline_axis_is_reverse() ? fragment_rect.left() : fragment_rect.right());
                else
                    rect.set_y(layout_node().inline_axis_is_reverse() ? fragment_rect.top() : fragment_rect.bottom());
                return rect;
            }
        }
    }

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

Optional<PaintableWithLines::CaretPaint> PaintableWithLines::resolve_caret_paint(InlinePaintable const* owner) const
{
    if (!should_paint_cursor())
        return {};

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
            return {};
        // Like the glyphs around it, the caret follows the text's own visibility, which may
        // differ from this box's.
        if (!layout_node_is_visible(fragment->style_source()))
            return {};
        caret_color = fragment->style_source().caret_color();
        cursor_rect = fragment->range_rect(SelectionState::StartAndEnd, cursor_position->offset(), cursor_position->offset());
    } else if (owner) {
        // Blank lines and empty editable elements are handled by the block / the box itself.
        return {};
    } else if (!is_visible()) {
        // Blank-line and empty-element carets belong to this block itself.
        return {};
    } else if (auto empty_line_rect = empty_line_caret_rect(*cursor_position); empty_line_rect.has_value()) {
        caret_color = m_fragments.first().style_source().caret_color();
        cursor_rect = { empty_line_rect->x(), empty_line_rect->y(), 1, empty_line_rect->height() };
    } else {
        // Empty editable elements have no fragments, but should still draw a cursor.
        if (cursor_position->node() != GC::Ptr { dom_node })
            return {};

        caret_color = layout_node().caret_color();
        cursor_rect = caret_rect_for_child_offset(cursor_position->offset());
    }

    return CaretPaint { cursor_rect, caret_color };
}

} // namespace Web::Painting
