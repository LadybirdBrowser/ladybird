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
    TRY(encoder.encode(node.url));
    TRY(encoder.encode(node.language));
    TRY(encoder.encode(node.bounds));
    TRY(encoder.encode(node.is_focused));
    TRY(encoder.encode(node.is_disabled));
    TRY(encoder.encode(node.heading_level));
    TRY(encoder.encode(node.live));
    TRY(encoder.encode(node.column_span));
    TRY(encoder.encode(node.row_span));
    TRY(encoder.encode(node.cell_row_index));
    TRY(encoder.encode(node.cell_column_index));
    TRY(encoder.encode(node.table_row_count));
    TRY(encoder.encode(node.table_column_count));
    TRY(encoder.encode(node.table_caption_id));
    TRY(encoder.encode(node.table_summary));
    TRY(encoder.encode(node.column_header_ids));
    TRY(encoder.encode(node.row_header_ids));
    TRY(encoder.encode(node.keybinding));
    TRY(encoder.encode(node.value_numeric));
    TRY(encoder.encode(node.value_minimum));
    TRY(encoder.encode(node.value_maximum));
    TRY(encoder.encode(node.value_step));
    TRY(encoder.encode(node.is_selected));
    TRY(encoder.encode(to_underlying(node.checked_state)));
    TRY(encoder.encode(to_underlying(node.expanded_state)));
    TRY(encoder.encode(node.is_editable));
    TRY(encoder.encode(node.is_multi_line));
    TRY(encoder.encode(node.is_read_only));
    TRY(encoder.encode(node.is_required));
    TRY(encoder.encode(node.is_invalid));
    TRY(encoder.encode(node.is_multi_selectable));
    TRY(encoder.encode(node.is_pressed));
    TRY(encoder.encode(node.is_visited));
    TRY(encoder.encode(node.caret_offset));
    TRY(encoder.encode(node.selection_start));
    TRY(encoder.encode(node.selection_end));
    TRY(encoder.encode(node.font_family));
    TRY(encoder.encode(node.font_size));
    TRY(encoder.encode(node.font_weight));
    TRY(encoder.encode(node.font_style));
    TRY(encoder.encode(node.color));
    TRY(encoder.encode(node.background_color));
    TRY(encoder.encode(node.text_decoration));
    TRY(encoder.encode(node.character_offsets));
    TRY(encoder.encode(node.line_break_character_offsets));
    TRY(encoder.encode(node.line_heights));
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
    auto url = TRY(decoder.decode<String>());
    auto language = TRY(decoder.decode<String>());
    auto bounds = TRY(decoder.decode<Gfx::IntRect>());
    auto is_focused = TRY(decoder.decode<bool>());
    auto is_disabled = TRY(decoder.decode<bool>());
    auto heading_level = TRY(decoder.decode<i32>());
    auto live = TRY(decoder.decode<String>());
    auto column_span = TRY(decoder.decode<i32>());
    auto row_span = TRY(decoder.decode<i32>());
    auto cell_row_index = TRY(decoder.decode<i32>());
    auto cell_column_index = TRY(decoder.decode<i32>());
    auto table_row_count = TRY(decoder.decode<i32>());
    auto table_column_count = TRY(decoder.decode<i32>());
    auto table_caption_id = TRY(decoder.decode<i64>());
    auto table_summary = TRY(decoder.decode<String>());
    auto column_header_ids = TRY(decoder.decode<Vector<i64>>());
    auto row_header_ids = TRY(decoder.decode<Vector<i64>>());
    auto keybinding = TRY(decoder.decode<String>());
    auto value_numeric = TRY(decoder.decode<double>());
    auto value_minimum = TRY(decoder.decode<double>());
    auto value_maximum = TRY(decoder.decode<double>());
    auto value_step = TRY(decoder.decode<double>());
    auto is_selected = TRY(decoder.decode<bool>());
    auto checked_state = static_cast<WebView::AccessibilityNodeData::CheckedState>(TRY(decoder.decode<u8>()));
    auto expanded_state = static_cast<WebView::AccessibilityNodeData::ExpandedState>(TRY(decoder.decode<u8>()));
    auto is_editable = TRY(decoder.decode<bool>());
    auto is_multi_line = TRY(decoder.decode<bool>());
    auto is_read_only = TRY(decoder.decode<bool>());
    auto is_required = TRY(decoder.decode<bool>());
    auto is_invalid = TRY(decoder.decode<bool>());
    auto is_multi_selectable = TRY(decoder.decode<bool>());
    auto is_pressed = TRY(decoder.decode<bool>());
    auto is_visited = TRY(decoder.decode<bool>());
    auto caret_offset = TRY(decoder.decode<i32>());
    auto selection_start = TRY(decoder.decode<i32>());
    auto selection_end = TRY(decoder.decode<i32>());
    auto font_family = TRY(decoder.decode<String>());
    auto font_size = TRY(decoder.decode<String>());
    auto font_weight = TRY(decoder.decode<String>());
    auto font_style = TRY(decoder.decode<String>());
    auto color = TRY(decoder.decode<String>());
    auto background_color = TRY(decoder.decode<String>());
    auto text_decoration = TRY(decoder.decode<String>());
    auto character_offsets = TRY(decoder.decode<Vector<Gfx::IntPoint>>());
    auto line_break_character_offsets = TRY(decoder.decode<Vector<i32>>());
    auto line_heights = TRY(decoder.decode<Vector<i32>>());

    return WebView::AccessibilityNodeData {
        id,
        parent_id,
        move(child_ids),
        move(role),
        move(name),
        move(description),
        move(value),
        move(url),
        move(language),
        bounds,
        is_focused,
        is_disabled,
        heading_level,
        move(live),
        column_span,
        row_span,
        cell_row_index,
        cell_column_index,
        table_row_count,
        table_column_count,
        table_caption_id,
        move(table_summary),
        move(column_header_ids),
        move(row_header_ids),
        move(keybinding),
        value_numeric,
        value_minimum,
        value_maximum,
        value_step,
        is_selected,
        checked_state,
        expanded_state,
        is_editable,
        is_multi_line,
        is_read_only,
        is_required,
        is_invalid,
        is_multi_selectable,
        is_pressed,
        is_visited,
        caret_offset,
        selection_start,
        selection_end,
        move(font_family),
        move(font_size),
        move(font_weight),
        move(font_style),
        move(color),
        move(background_color),
        move(text_decoration),
        move(character_offsets),
        move(line_break_character_offsets),
        move(line_heights),
    };
}
