/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/WebView/AccessibilityNodeData.h>

namespace WebView {

unsigned AccessibilityTextStyle::hash() const
{
    auto hash = font_family.hash();
    for (auto const* string : { &font_size, &font_weight, &font_style, &color, &background_color, &text_decoration })
        hash = pair_int_hash(hash, string->hash());
    return hash;
}

AccessibilityTextGeometry AccessibilityTextGeometry::create(ReadonlySpan<Gfx::IntPoint> character_offsets, Vector<i32> line_break_character_offsets, Vector<i32> line_tops, Vector<i32> line_heights)
{
    AccessibilityTextGeometry geometry;
    geometry.line_break_character_offsets = move(line_break_character_offsets);
    geometry.line_tops = move(line_tops);
    geometry.line_heights = move(line_heights);
    geometry.character_x.ensure_capacity(character_offsets.size());
    bool every_y_is_its_line_top = true;
    for (size_t i = 0; i < character_offsets.size(); ++i) {
        geometry.character_x.unchecked_append(character_offsets[i].x());
        auto line = geometry.line_of(i);
        if (line >= geometry.line_tops.size() || geometry.line_tops[line] != character_offsets[i].y())
            every_y_is_its_line_top = false;
    }
    if (!every_y_is_its_line_top) {
        geometry.character_y.ensure_capacity(character_offsets.size());
        for (auto const& offset : character_offsets)
            geometry.character_y.unchecked_append(offset.y());
    }
    return geometry;
}

size_t AccessibilityTextGeometry::line_of(size_t index) const
{
    // Line starts only ever increase, so the line is the one before the first that starts after the character.
    size_t first_after = 0;
    size_t end = line_break_character_offsets.size();
    while (first_after < end) {
        auto middle = first_after + (end - first_after) / 2;
        if (static_cast<i64>(line_break_character_offsets[middle]) <= static_cast<i64>(index))
            first_after = middle + 1;
        else
            end = middle;
    }
    return first_after > 0 ? first_after - 1 : 0;
}

Gfx::IntPoint AccessibilityTextGeometry::character_offset(size_t index) const
{
    if (index >= character_x.size())
        return {};
    if (index < character_y.size())
        return { character_x[index], character_y[index] };
    auto line = line_of(index);
    return { character_x[index], line < line_tops.size() ? line_tops[line] : 0 };
}

}

template<typename T>
static ErrorOr<void> encode_shared(IPC::Encoder& encoder, WebView::AccessibilityShared<T> const& shared)
{
    TRY(encoder.encode(!shared.is_default()));
    if (!shared.is_default())
        TRY(encoder.encode(*shared));
    return {};
}

template<typename T>
static ErrorOr<WebView::AccessibilityShared<T>> decode_shared(IPC::Decoder& decoder)
{
    WebView::AccessibilityShared<T> shared;
    if (TRY(decoder.decode<bool>()))
        shared.set(TRY(decoder.decode<T>()));
    return shared;
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::AccessibilityNodeData const& node)
{
    TRY(encoder.encode(node.id));
    TRY(encoder.encode(node.parent_id));
    TRY(encoder.encode(node.child_ids));
    TRY(encoder.encode(node.role));
    TRY(encoder.encode(node.name));
    TRY(encoder.encode(node.bounds));
    TRY(encoder.encode(node.is_focused));
    TRY(encoder.encode(node.is_disabled));
    TRY(encoder.encode(node.heading_level));
    TRY(encoder.encode(node.hierarchical_level));
    TRY(encoder.encode(node.is_selected));
    TRY(encoder.encode(node.checked_state));
    TRY(encoder.encode(node.expanded_state));
    TRY(encoder.encode(node.is_editable));
    TRY(encoder.encode(node.is_multi_line));
    TRY(encoder.encode(node.is_password));
    TRY(encoder.encode(node.is_read_only));
    TRY(encoder.encode(node.is_required));
    TRY(encoder.encode(node.is_invalid));
    TRY(encoder.encode(node.is_multi_selectable));
    TRY(encoder.encode(node.pressed_state));
    TRY(encoder.encode(node.is_visited));
    TRY(encoder.encode(node.caret_offset));
    TRY(encoder.encode(node.selection_start));
    TRY(encoder.encode(node.selection_end));
    TRY(encode_shared(encoder, node.details));
    TRY(encode_shared(encoder, node.text_style));
    TRY(encode_shared(encoder, node.text_geometry));
    return {};
}

template<>
ErrorOr<WebView::AccessibilityNodeData> IPC::decode(Decoder& decoder)
{
    WebView::AccessibilityNodeData node;
    node.id = TRY(decoder.decode<i64>());
    node.parent_id = TRY(decoder.decode<i64>());
    node.child_ids = TRY(decoder.decode<Vector<i64>>());
    node.role = TRY(decoder.decode<String>());
    node.name = TRY(decoder.decode<String>());
    node.bounds = TRY(decoder.decode<Gfx::IntRect>());
    node.is_focused = TRY(decoder.decode<bool>());
    node.is_disabled = TRY(decoder.decode<bool>());
    node.heading_level = TRY(decoder.decode<i32>());
    node.hierarchical_level = TRY(decoder.decode<i32>());
    node.is_selected = TRY(decoder.decode<bool>());
    node.checked_state = TRY(decoder.decode<WebView::AccessibilityNodeData::CheckedState>());
    node.expanded_state = TRY(decoder.decode<WebView::AccessibilityNodeData::ExpandedState>());
    node.is_editable = TRY(decoder.decode<bool>());
    node.is_multi_line = TRY(decoder.decode<bool>());
    node.is_password = TRY(decoder.decode<bool>());
    node.is_read_only = TRY(decoder.decode<bool>());
    node.is_required = TRY(decoder.decode<bool>());
    node.is_invalid = TRY(decoder.decode<bool>());
    node.is_multi_selectable = TRY(decoder.decode<bool>());
    node.pressed_state = TRY(decoder.decode<WebView::AccessibilityNodeData::PressedState>());
    node.is_visited = TRY(decoder.decode<bool>());
    node.caret_offset = TRY(decoder.decode<i32>());
    node.selection_start = TRY(decoder.decode<i32>());
    node.selection_end = TRY(decoder.decode<i32>());
    node.details = TRY(decode_shared<WebView::AccessibilityNodeDetails>(decoder));
    node.text_style = TRY(decode_shared<WebView::AccessibilityTextStyle>(decoder));
    node.text_geometry = TRY(decode_shared<WebView::AccessibilityTextGeometry>(decoder));
    return node;
}
