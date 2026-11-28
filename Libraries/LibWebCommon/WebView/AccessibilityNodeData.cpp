/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibIPC/Message.h>
#include <LibWebCommon/WebView/AccessibilityNodeData.h>

namespace WebView {

unsigned AccessibilityTextStyle::hash() const
{
    auto hash = font_family.hash();
    for (auto const* string : { &font_size, &font_weight, &font_style, &color, &background_color, &text_decoration })
        hash = pair_int_hash(hash, string->hash());
    return hash;
}

AccessibilityTextGeometry AccessibilityTextGeometry::create(ReadonlySpan<Gfx::IntPoint> character_offsets, ReadonlySpan<i32> line_break_character_offsets, ReadonlySpan<i32> line_tops, ReadonlySpan<i32> line_heights)
{
    VERIFY(line_tops.size() == line_break_character_offsets.size() && line_heights.size() == line_break_character_offsets.size());
    auto line_count = line_break_character_offsets.size();

    AccessibilityTextGeometry geometry;
    auto& data = geometry.data;
    data.ensure_capacity(HEADER_SIZE + 3 * line_count + character_offsets.size());
    data.unchecked_append(static_cast<i32>(line_count));
    data.unchecked_append(0);
    data.unchecked_append(line_break_character_offsets.data(), line_count);
    data.unchecked_append(line_tops.data(), line_count);
    data.unchecked_append(line_heights.data(), line_count);
    for (auto const& offset : character_offsets)
        data.unchecked_append(offset.x());

    for (size_t i = 0; i < character_offsets.size(); ++i) {
        auto line = geometry.line_of(i);
        if (line < line_count && line_tops[line] == character_offsets[i].y())
            continue;
        data[1] = 1;
        data.ensure_capacity(data.size() + character_offsets.size());
        for (auto const& offset : character_offsets)
            data.unchecked_append(offset.y());
        break;
    }
    return geometry;
}

AccessibilityTextGeometry::Layout AccessibilityTextGeometry::layout() const
{
    if (data.size() < HEADER_SIZE || data[0] < 0 || (data[1] != 0 && data[1] != 1))
        return {};
    auto line_count = static_cast<size_t>(data[0]);
    auto has_character_y = data[1] == 1;
    if (line_count > (data.size() - HEADER_SIZE) / 3)
        return {};
    auto rest = data.size() - HEADER_SIZE - 3 * line_count;
    if (has_character_y && rest % 2 != 0)
        return {};
    return { line_count, has_character_y ? rest / 2 : rest, has_character_y };
}

ReadonlySpan<i32> AccessibilityTextGeometry::section(size_t index) const
{
    auto line_count = layout().line_count;
    if (line_count == 0)
        return {};
    return data.span().slice(HEADER_SIZE + index * line_count, line_count);
}

size_t AccessibilityTextGeometry::line_of(size_t index) const
{
    // Line starts only ever increase, so the line is the one before the first that starts after the character.
    auto line_starts = line_break_character_offsets();
    size_t first_after = 0;
    size_t end = line_starts.size();
    while (first_after < end) {
        auto middle = first_after + (end - first_after) / 2;
        if (static_cast<i64>(line_starts[middle]) <= static_cast<i64>(index))
            first_after = middle + 1;
        else
            end = middle;
    }
    return first_after > 0 ? first_after - 1 : 0;
}

Gfx::IntPoint AccessibilityTextGeometry::character_offset(size_t index) const
{
    auto [line_count, character_count, has_character_y] = layout();
    if (index >= character_count)
        return {};
    auto x_start = HEADER_SIZE + 3 * line_count;
    auto x = data[x_start + index];
    if (has_character_y)
        return { x, data[x_start + character_count + index] };
    auto line = line_of(index);
    return { x, line < line_count ? line_tops()[line] : 0 };
}

template<typename T>
static ErrorOr<Core::AnonymousBuffer> encode_into_shared_memory(T const& value)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    TRY(encoder.encode(value));
    auto const& bytes = message.data();
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(bytes.size()));
    __builtin_memcpy(buffer.data<u8>(), bytes.data(), bytes.size());
    return buffer;
}

template<typename T>
static ErrorOr<T> decode_from_shared_memory(Core::AnonymousBuffer const& buffer)
{
    if (!buffer.is_valid())
        return Error::from_string_literal("Encoded accessibility data has no buffer");
    TRY(buffer.validate_backing_size());
    FixedMemoryStream stream { ReadonlyBytes { buffer.data<u8>(), buffer.size() } };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<T>();
}

ErrorOr<EncodedAccessibilityTree> EncodedAccessibilityTree::encode(Vector<AccessibilityNodeData> const& nodes)
{
    return EncodedAccessibilityTree { TRY(encode_into_shared_memory(nodes)) };
}

ErrorOr<Vector<AccessibilityNodeData>> EncodedAccessibilityTree::decode() const
{
    return decode_from_shared_memory<Vector<AccessibilityNodeData>>(buffer);
}

ErrorOr<EncodedAccessibilityTreeDelta> EncodedAccessibilityTreeDelta::encode(AccessibilityTreeDelta const& delta)
{
    return EncodedAccessibilityTreeDelta { TRY(encode_into_shared_memory(delta)) };
}

ErrorOr<AccessibilityTreeDelta> EncodedAccessibilityTreeDelta::decode() const
{
    return decode_from_shared_memory<AccessibilityTreeDelta>(buffer);
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
    TRY(encoder.encode(node.role.to_string()));
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
    node.role = FlyString { TRY(decoder.decode<String>()) };
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

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::EncodedAccessibilityTree const& encoded)
{
    return encoder.encode(encoded.buffer);
}

template<>
ErrorOr<WebView::EncodedAccessibilityTree> IPC::decode(Decoder& decoder)
{
    return WebView::EncodedAccessibilityTree { TRY(decoder.decode<Core::AnonymousBuffer>()) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::EncodedAccessibilityTreeDelta const& encoded)
{
    return encoder.encode(encoded.buffer);
}

template<>
ErrorOr<WebView::EncodedAccessibilityTreeDelta> IPC::decode(Decoder& decoder)
{
    return WebView::EncodedAccessibilityTreeDelta { TRY(decoder.decode<Core::AnonymousBuffer>()) };
}
