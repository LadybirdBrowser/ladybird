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

BorderRadiiData InlinePaintable::piece_border_radii_data(CSSPixelSize border_box_size, u8 present_edges) const
{
    if (!layout_node().has_noninitial_border_radii())
        return {};

    constexpr u8 top_edge_bit = 1 << 0;
    constexpr u8 right_edge_bit = 1 << 1;
    constexpr u8 bottom_edge_bit = 1 << 2;
    constexpr u8 left_edge_bit = 1 << 3;
    auto top_edge_is_cut = !(present_edges & top_edge_bit);
    auto right_edge_is_cut = !(present_edges & right_edge_bit);
    auto bottom_edge_is_cut = !(present_edges & bottom_edge_bit);
    auto left_edge_is_cut = !(present_edges & left_edge_bit);

    CSSPixelRect const border_rect { 0, 0, border_box_size.width(), border_box_size.height() };
    auto top_left = top_edge_is_cut || left_edge_is_cut ? CSS::BorderRadiusData {} : layout_node().border_top_left_radius();
    auto top_right = top_edge_is_cut || right_edge_is_cut ? CSS::BorderRadiusData {} : layout_node().border_top_right_radius();
    auto bottom_right = bottom_edge_is_cut || right_edge_is_cut ? CSS::BorderRadiusData {} : layout_node().border_bottom_right_radius();
    auto bottom_left = bottom_edge_is_cut || left_edge_is_cut ? CSS::BorderRadiusData {} : layout_node().border_bottom_left_radius();
    return normalize_border_radii_data(border_rect, border_rect, top_left, top_right, bottom_right, bottom_left);
}

bool InlinePaintable::has_content_pieces() const
{
    return Layout::RustFFI::layout_arena_inline_paintable_has_content_pieces(rust_arena().handle(), rust_slot());
}

CSSPixelPoint InlinePaintable::box_type_agnostic_position() const
{
    auto result = Layout::RustFFI::layout_arena_inline_paintable_first_piece_position(rust_arena().handle(), rust_slot());
    if (!result.has_value)
        return absolute_position();
    return { CSSPixels::from_raw(result.x), CSSPixels::from_raw(result.y) };
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
