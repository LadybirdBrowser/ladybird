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
    return has_content_pieces() || Layout::RustFFI::layout_arena_paintable_has_child_paintables(rust_arena().handle(), rust_slot());
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

}
