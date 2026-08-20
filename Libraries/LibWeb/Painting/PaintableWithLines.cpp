/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Painting/InlinePaintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Painting {

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

InlinePaintable const* nearest_self_painting_inline_box(Layout::Node const& node)
{
    for (auto const* ancestor = node.nearest_fragmented_inline_ancestor(); ancestor; ancestor = ancestor->nearest_fragmented_inline_ancestor()) {
        auto const* proxy = as_if<InlinePaintable>(ancestor->paintable().ptr());
        if (proxy && proxy->is_self_painting())
            return proxy;
    }
    return nullptr;
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
    if (offset > 0) {
        auto* previous_child = dom_node->child_at_index(offset - 1);
        auto const* previous_layout_node = previous_child ? previous_child->unsafe_layout_node() : nullptr;
        if (previous_layout_node && previous_layout_node->is_atomic_inline()) {
            auto result = Layout::RustFFI::layout_arena_paintable_first_fragment_rect_for_node(rust_arena().handle(), rust_slot(), Layout::Node::slot_id(previous_layout_node));
            if (result.has_value) {
                auto fragment_rect = from_ffi_css_pixel_rect(result.rect);
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
    struct PrecedingContentContext {
        GC::Ref<DOM::Node> child;
        Optional<CSSPixels> preceding_content_bottom;
    } preceding_context { const_cast<DOM::Node&>(*child), {} };
    Layout::RustFFI::layout_arena_for_each_subtree_fragment_rect(
        rust_arena().handle(), rust_slot(), &preceding_context,
        [](void* context_pointer, void* fragment_layout_node_shell, Layout::RustFFI::FfiCssPixelRect rect) {
            auto& context = *static_cast<PrecedingContentContext*>(context_pointer);
            auto const* fragment_layout_node = static_cast<Layout::Node const*>(fragment_layout_node_shell);
            auto* fragment_dom_node = fragment_layout_node ? const_cast<DOM::Node*>(fragment_layout_node->dom_node()) : nullptr;
            if (!fragment_dom_node || !(context.child->compare_document_position(fragment_dom_node) & DOM::Node::DOCUMENT_POSITION_PRECEDING))
                return;
            auto bottom = CSSPixels::from_raw(rect.y) + CSSPixels::from_raw(rect.height);
            if (!context.preceding_content_bottom.has_value() || bottom > *context.preceding_content_bottom)
                context.preceding_content_bottom = bottom;
        });
    auto& preceding_content_bottom = preceding_context.preceding_content_bottom;

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

    Vector<Layout::RustFFI::NodeSlotId, 2> text_slots;
    if (auto const* text = as_if<DOM::Text>(cursor_position->node().ptr()))
        text_slots = Layout::TextOffsetMapping { *text }.slot_ids();

    if (!text_slots.is_empty()) {
        auto result = Layout::RustFFI::layout_arena_text_caret_rect_for_position(
            rust_arena().handle(), text_slots.data(), text_slots.size(), cursor_position->offset(),
            cursor_position->affinity() == TextAffinity::Downstream);
        if (result.found && result.owner_paintable == static_cast<void const*>(static_cast<Paintable const*>(this))) {
            if (result.nearest_self_painting_inline != static_cast<void const*>(static_cast<Paintable const*>(owner)))
                return {};
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(result.style_source);
            if (!style_source || !layout_node_is_visible(*style_source))
                return {};
            return CaretPaint { from_ffi_css_pixel_rect(result.rect), style_source->caret_color() };
        }
        if (result.found) {
            return {};
        }
    }

    if (owner) {
        // Blank lines and empty editable elements are handled by the block / the box itself.
        return {};
    }
    if (!is_visible()) {
        // Blank-line and empty-element carets belong to this block itself.
        return {};
    }

    if (!text_slots.is_empty()) {
        auto empty_line = Layout::RustFFI::layout_arena_paintable_empty_line_caret_rect(
            rust_arena().handle(), rust_slot(), text_slots.data(), text_slots.size(), cursor_position->offset());
        if (empty_line.has_value) {
            auto const* style_source = static_cast<Layout::NodeWithStyle const*>(empty_line.style_source);
            if (!style_source)
                return {};
            auto empty_line_rect = from_ffi_css_pixel_rect(empty_line.rect);
            CSSPixelRect cursor_rect { empty_line_rect.x(), empty_line_rect.y(), 1, empty_line_rect.height() };
            return CaretPaint { cursor_rect, style_source->caret_color() };
        }
    }

    if (cursor_position->node() != GC::Ptr { dom_node })
        return {};

    return CaretPaint { caret_rect_for_child_offset(cursor_position->offset()), layout_node().caret_color() };
}

} // namespace Web::Painting
