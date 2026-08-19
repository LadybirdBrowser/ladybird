/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/InlinePaintable.h>

namespace Web::Painting {

NonnullRefPtr<InlinePaintable> InlinePaintable::create(Layout::NodeWithStyle const& layout_node)
{
    return adopt_ref(*new InlinePaintable(layout_node));
}

InlinePaintable::InlinePaintable(Layout::NodeWithStyle const& layout_node)
    : Paintable(layout_node)
{
}

InlinePaintable::~InlinePaintable() = default;

void InlinePaintable::reset_for_relayout()
{
    Paintable::reset_for_relayout();
    m_piece_indices.clear();
    m_local_padding_box_union = {};
    m_local_border_box_union = {};
}

PaintableWithLines const* InlinePaintable::inline_root() const
{
    return as_if<PaintableWithLines>(containing_block().ptr());
}

CSSPixelRect InlinePaintable::absolute_piece_border_box_rect(InlineBoxPiece const& piece) const
{
    auto rect = piece.border_box_rect;
    if (auto const* root = inline_root())
        rect.translate_by(root->absolute_position());
    return rect;
}

CSSPixelRect InlinePaintable::piece_padding_box_rect(InlineBoxPiece const& piece, CSSPixelRect const& border_box_rect) const
{
    return piece.shrunken_by_present_edges(border_box_rect, box_model().border);
}

CSSPixelRect InlinePaintable::piece_content_box_rect(InlineBoxPiece const& piece, CSSPixelRect const& border_box_rect) const
{
    return piece.shrunken_by_present_edges(piece_padding_box_rect(piece, border_box_rect), box_model().padding);
}

BorderRadiiData InlinePaintable::piece_border_radii_data(InlineBoxPiece const& piece) const
{
    using Edge = InlineBoxPiece::Edge;
    if (!layout_node().has_noninitial_border_radii())
        return {};

    auto top_edge_is_cut = !piece.has_edge(Edge::Top);
    auto bottom_edge_is_cut = !piece.has_edge(Edge::Bottom);
    auto left_edge_is_cut = !piece.has_edge(Edge::Left);
    auto right_edge_is_cut = !piece.has_edge(Edge::Right);

    CSSPixelRect const border_rect { 0, 0, piece.border_box_rect.width(), piece.border_box_rect.height() };
    auto top_left = top_edge_is_cut || left_edge_is_cut ? CSS::BorderRadiusData {} : layout_node().border_top_left_radius();
    auto top_right = top_edge_is_cut || right_edge_is_cut ? CSS::BorderRadiusData {} : layout_node().border_top_right_radius();
    auto bottom_right = bottom_edge_is_cut || right_edge_is_cut ? CSS::BorderRadiusData {} : layout_node().border_bottom_right_radius();
    auto bottom_left = bottom_edge_is_cut || left_edge_is_cut ? CSS::BorderRadiusData {} : layout_node().border_bottom_left_radius();
    return normalize_border_radii_data(border_rect, border_rect, top_left, top_right, bottom_right, bottom_left);
}

bool InlinePaintable::has_content_pieces() const
{
    auto const* root = inline_root();
    if (!root)
        return false;
    auto const& pieces = root->inline_box_pieces();
    for (auto piece_index : m_piece_indices) {
        if (!pieces[piece_index].is_geometry_only_placeholder)
            return true;
    }
    return false;
}

CSSPixelPoint InlinePaintable::box_type_agnostic_position() const
{
    auto const* root = inline_root();
    if (!root || m_piece_indices.is_empty())
        return absolute_position();
    auto const& piece = root->inline_box_pieces()[m_piece_indices.first()];
    auto rect = piece.is_geometry_only_placeholder ? piece.border_box_rect : piece_content_box_rect(piece, piece.border_box_rect);
    return rect.location().translated(root->absolute_position());
}

CSSPixelRect InlinePaintable::compute_absolute_padding_box_rect() const
{
    return m_local_padding_box_union.translated(absolute_rect().location());
}

CSSPixelRect InlinePaintable::compute_absolute_border_box_rect() const
{
    return m_local_border_box_union.translated(absolute_rect().location());
}

bool InlinePaintable::has_content() const
{
    // Interrupting block-in-inline children produce only placeholder pieces, so any child
    // paintable also counts as content.
    return has_content_pieces() || has_children();
}

Optional<PaintableWithLines::CaretPaint> InlinePaintable::resolve_empty_editable_caret_paint() const
{
    if (has_content())
        return {};

    if (!should_paint_cursor())
        return {};

    auto cursor_position = document().cursor_position();
    VERIFY(cursor_position);

    auto const* dom_node = layout_node().dom_node();
    if (!dom_node || cursor_position->node() != GC::Ptr { dom_node })
        return {};

    auto position = box_type_agnostic_position();
    return PaintableWithLines::CaretPaint {
        .rect = { position.x(), position.y(), 1, layout_node().line_height() },
        .color = layout_node().caret_color(),
    };
}

void InlinePaintable::set_needs_repaint(InvalidateDisplayList should_invalidate_display_list)
{
    Paintable::set_needs_repaint(should_invalidate_display_list);

    if (should_invalidate_display_list == InvalidateDisplayList::Yes) {
        // This box's glyphs are recorded in an ancestor's foreground commands: the containing
        // block's, or a self-painting inline ancestor's.
        for (auto ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
            ancestor->invalidate_paint_cache();
            if (is<PaintableWithLines>(*ancestor))
                break;
        }
    }
}

}
