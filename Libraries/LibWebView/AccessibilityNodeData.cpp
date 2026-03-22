/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/AccessibilityNodeData.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::AccessibilityNodeData const& node)
{
    TRY(encoder.encode(node.id));
    TRY(encoder.encode(node.parent_id));
    TRY(encoder.encode(node.child_ids));
    TRY(encoder.encode(node.role));
    TRY(encoder.encode(node.name));
    TRY(encoder.encode(node.description));
    TRY(encoder.encode(node.value));
    TRY(encoder.encode(node.bounds));
    TRY(encoder.encode(node.is_focused));
    TRY(encoder.encode(node.is_disabled));
    TRY(encoder.encode(node.heading_level));
    return {};
}

template<>
ErrorOr<WebView::AccessibilityNodeData> IPC::decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<i64>());
    auto parent_id = TRY(decoder.decode<i64>());
    auto child_ids = TRY(decoder.decode<Vector<i64>>());
    auto role = TRY(decoder.decode<String>());
    auto name = TRY(decoder.decode<String>());
    auto description = TRY(decoder.decode<String>());
    auto value = TRY(decoder.decode<String>());
    auto bounds = TRY(decoder.decode<Gfx::IntRect>());
    auto is_focused = TRY(decoder.decode<bool>());
    auto is_disabled = TRY(decoder.decode<bool>());
    auto heading_level = TRY(decoder.decode<i32>());
    auto live = TRY(decoder.decode<String>());

    return WebView::AccessibilityNodeData {
        id,
        parent_id,
        move(child_ids),
        move(role),
        move(name),
        move(description),
        move(value),
        bounds,
        is_focused,
        is_disabled,
        heading_level,
        move(live),
    };
}
