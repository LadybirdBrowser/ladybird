/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/StackingContext.h>

namespace Web::Painting {

NonnullRefPtr<InlinePaintable> InlinePaintable::create(Layout::NodeWithStyleAndBoxModelMetrics const& layout_node)
{
    return adopt_ref(*new InlinePaintable(layout_node));
}

InlinePaintable::InlinePaintable(Layout::NodeWithStyleAndBoxModelMetrics const& layout_node)
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
    m_fragment_ownership_filter = {};
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
    auto const& computed_values = this->computed_values();
    if (!computed_values.has_noninitial_border_radii())
        return {};

    auto top_edge_is_cut = !piece.has_edge(Edge::Top);
    auto bottom_edge_is_cut = !piece.has_edge(Edge::Bottom);
    auto left_edge_is_cut = !piece.has_edge(Edge::Left);
    auto right_edge_is_cut = !piece.has_edge(Edge::Right);

    CSSPixelRect const border_rect { 0, 0, piece.border_box_rect.width(), piece.border_box_rect.height() };
    auto top_left = top_edge_is_cut || left_edge_is_cut ? CSS::BorderRadiusData {} : computed_values.border_top_left_radius();
    auto top_right = top_edge_is_cut || right_edge_is_cut ? CSS::BorderRadiusData {} : computed_values.border_top_right_radius();
    auto bottom_right = bottom_edge_is_cut || right_edge_is_cut ? CSS::BorderRadiusData {} : computed_values.border_bottom_right_radius();
    auto bottom_left = bottom_edge_is_cut || left_edge_is_cut ? CSS::BorderRadiusData {} : computed_values.border_bottom_left_radius();
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

void InlinePaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    auto const* root = inline_root();
    if (!root)
        return;

    auto root_position = root->absolute_position();

    if (phase == PaintPhase::Background && is_visible()) {
        paint_backdrop_filter(context);
        bool background_is_propagated_to_root = body_background_is_propagated_to_root(layout_node());
        auto has_borders = has_css_borders();
        for_each_piece([&](auto const& piece) {
            if (piece.is_geometry_only_placeholder)
                return;
            auto border_box_rect = piece.border_box_rect.translated(root_position);
            auto padding_box_rect = piece_padding_box_rect(piece, border_box_rect);
            auto border_radii = piece_border_radii_data(piece);
            if (!background_is_propagated_to_root)
                paint_background_within(context, has_borders ? border_box_rect : padding_box_rect, border_radii);
            paint_box_shadow(context, border_box_rect, padding_box_rect, border_radii);
        });
    }

    if (phase == PaintPhase::Border && is_visible()) {
        using Edge = InlineBoxPiece::Edge;
        auto const& computed_values = this->computed_values();
        auto const& border = box_model().border;
        for_each_piece([&](auto const& piece) {
            if (piece.is_geometry_only_placeholder)
                return;
            auto borders_data = BordersData {
                .top = border.top == 0 || !piece.has_edge(Edge::Top) ? CSS::BorderData() : computed_values.border_top(),
                .right = border.right == 0 || !piece.has_edge(Edge::Right) ? CSS::BorderData() : computed_values.border_right(),
                .bottom = border.bottom == 0 || !piece.has_edge(Edge::Bottom) ? CSS::BorderData() : computed_values.border_bottom(),
                .left = border.left == 0 || !piece.has_edge(Edge::Left) ? CSS::BorderData() : computed_values.border_left(),
            };
            paint_border(context, piece.border_box_rect.translated(root_position), borders_data, piece_border_radii_data(piece));
        });
    }

    if (phase == PaintPhase::Outline && is_visible()) {
        for_each_piece([&](auto const& piece) {
            if (piece.is_geometry_only_placeholder)
                return;
            paint_outline(context, piece.border_box_rect.translated(root_position), piece_border_radii_data(piece));
        });
    }

    if (phase == PaintPhase::Foreground) {
        // Fragments (and the caret between their glyphs) are not gated on this box being
        // visible: descendants may set visibility: visible again under a hidden box, so each
        // fragment is filtered by its own node's visibility.
        if (is_self_painting()) {
            root->paint_fragments_foreground(context, m_fragment_ownership_filter);
            if (document().cursor_position())
                root->paint_cursor(context, this);
        }
        if (is_visible() && document().cursor_position())
            paint_empty_editable_cursor(context);
    }
}

bool InlinePaintable::has_content() const
{
    // Interrupting block-in-inline children produce only placeholder pieces, so any child
    // paintable also counts as content.
    return has_content_pieces() || has_children();
}

void InlinePaintable::paint_empty_editable_cursor(DisplayListRecordingContext& context) const
{
    if (has_content())
        return;

    if (!should_paint_cursor())
        return;

    auto cursor_position = document().cursor_position();
    VERIFY(cursor_position);

    auto const* dom_node = layout_node().dom_node();
    if (!dom_node || cursor_position->node() != dom_node)
        return;

    auto caret_color = computed_values().caret_color();
    if (caret_color.alpha() == 0)
        return;

    auto position = box_type_agnostic_position();
    CSSPixelRect cursor_rect { position.x(), position.y(), 1, computed_values().line_height() };
    context.display_list_recorder().fill_rect(context.rounded_device_rect(cursor_rect).to_type<int>(), caret_color);
}

void InlinePaintable::record_hit_test_items(DisplayListRecordingContext& context, PaintPhase phase) const
{
    auto* hit_test_display_list = context.hit_test_display_list();
    if (!hit_test_display_list)
        return;

    if (layout_node().is_anonymous() && !layout_node().is_generated_for_pseudo_element())
        return;

    // Only this box's own items are gated on its visibility; the fragments it owns may belong
    // to descendants that override visibility or pointer-events, so they are filtered per
    // fragment instead.
    bool box_itself_is_hit_testable = is_visible() && visible_for_hit_testing();

    auto append_piece_boxes = [&] {
        for_each_piece([&](auto const& piece) {
            if (piece.is_geometry_only_placeholder)
                return;
            hit_test_display_list->append_box(*this, const_cast<InlinePaintable&>(*this), absolute_piece_border_box_rect(piece), accumulated_visual_context_index(), piece_border_radii_data(piece));
        });
    };

    if (phase == PaintPhase::Background) {
        if (box_itself_is_hit_testable)
            append_piece_boxes();
        return;
    }

    if (phase != PaintPhase::Foreground)
        return;

    if (is_self_painting()) {
        auto const* root = inline_root();
        if (!root)
            return;
        auto const& fragments = root->fragments();

        // Hit-test precedence follows paint order: this box's own text loses to the box
        // itself (re-recorded so its z-order matches this box's paint order), while nested
        // content (e.g. a link inside this box) wins over it.
        Vector<size_t, 8> nested_content_indices;
        m_fragment_ownership_filter.for_each_owned_fragment_index(fragments.size(), [&](size_t index) {
            auto const& fragment = fragments[index];
            if (fragment.is_block_level_box())
                return;
            if (fragment.layout_node().nearest_fragmented_inline_ancestor() == &layout_node())
                hit_test_display_list->append_text_fragment(fragment, accumulated_visual_context_for_descendants_index());
            else
                nested_content_indices.append(index);
        });
        if (box_itself_is_hit_testable && stacking_context())
            append_piece_boxes();
        for (auto index : nested_content_indices)
            hit_test_display_list->append_text_fragment(fragments[index], accumulated_visual_context_for_descendants_index());
    }

    // Clicks must be able to place the caret in an editable element with no content, so its
    // target is recorded in foreground order, winning over earlier-painted overlapping content.
    if (box_itself_is_hit_testable && !has_content())
        record_empty_editable_hit_test_item(*hit_test_display_list);
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
