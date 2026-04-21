/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API AccessibilityNodeData {
    i64 id { 0 };
    i64 parent_id { -1 };
    Vector<i64> child_ids;

    String role;
    String name;
    String description;
    String value;

    // For link elements, the href URL. Empty for non-links.
    String url;

    // For the document root, the IETF BCP 47 language tag from the html[lang] attribute. Empty otherwise.
    String language;

    Gfx::IntRect bounds;

    bool is_focused { false };
    bool is_disabled { false };
    i32 heading_level { 0 };

    String live; // "assertive", "polite", or empty

    i32 column_span { 1 };
    i32 row_span { 1 };

    // For AtkTableCell: 0-based row/column index of this cell within its table. -1 for non-cells.
    i32 cell_row_index { -1 };
    i32 cell_column_index { -1 };

    // For AtkTable: row/column counts of this table. -1 for non-tables.
    i32 table_row_count { -1 };
    i32 table_column_count { -1 };

    // For AtkTable: IDs of the caption and summary elements (non-table nodes), and per-column/per-row header cell IDs.
    // Empty / -1 when not applicable.
    i64 table_caption_id { -1 };
    String table_summary;
    Vector<i64> column_header_ids;
    Vector<i64> row_header_ids;

    // Keyboard shortcut (accesskey attribute) for AtkAction::get_keybinding. Empty if none.
    String keybinding;

    // AtkValue fields for sliders, progress bars, meters, spinbuttons. NaN when not applicable.
    double value_numeric { AK::NaN<double> };
    double value_minimum { AK::NaN<double> };
    double value_maximum { AK::NaN<double> };
    double value_step { AK::NaN<double> };

    // Whether this option-like node is currently selected (listbox option, menu item, tab, etc.).
    bool is_selected { false };

    // State flags surfaced via AtkStateSet. Each corresponds to an ATK_STATE_* constant.
    enum class CheckedState : u8 {
        NotApplicable,
        Unchecked,
        Checked,
        Mixed,
    };
    CheckedState checked_state { CheckedState::NotApplicable };

    // Tri-state expansion: NotApplicable for non-expandable elements, Collapsed/Expanded for details element,
    // aria-expanded, tree nodes, etc.
    enum class ExpandedState : u8 {
        NotApplicable,
        Collapsed,
        Expanded,
    };
    ExpandedState expanded_state { ExpandedState::NotApplicable };

    bool is_editable { false };         // Text input, textarea, contenteditable
    bool is_multi_line { false };       // textarea element or contenteditable block
    bool is_read_only { false };        // "readonly" attribute or aria-readonly=true
    bool is_required { false };         // "required" attribute or aria-required=true
    bool is_invalid { false };          // aria-invalid=true
    bool is_multi_selectable { false }; // select[multiple] or aria-multiselectable=true
    bool is_pressed { false };          // aria-pressed on toggle buttons
    bool is_visited { false };          // Visited links

    // Caret position within this node's text, as a Unicode character offset. -1 if the caret is not in this node.
    // Populated from the DOM Selection for the node containing the selection focus (or the collapsed-selection caret —
    // that is, no text selected and cursor positioned here).
    i32 caret_offset { -1 };

    // Text-selection range within this node's text, as Unicode character offsets. Both are -1 if there is no selection
    // in this node. When set, [selection_start, selection_end) describes the selected range.
    i32 selection_start { -1 };
    i32 selection_end { -1 };

    // Text-formatting attributes for text leaf nodes, read from the parent element's computed style. Stored as strings
    // in the format expected by AT-SPI2. Empty on non-text-leaf nodes.
    String font_family;
    String font_size;   // CSS pixel size as a decimal string; e.g., "16"
    String font_weight; // Numeric weight as decimal; e.g., "400" / "700"
    String font_style;  // "normal" / "italic" / "oblique"
    String color;       // e.g., "#000000"
    String background_color;
    String text_decoration; // "none" / "underline" / "line-through" / space-separated combinations

    // For text leaf nodes, per-character (x, y) offsets in CSS pixels, relative to the text's left edge and the top of
    // the text's first line. Length equals the character count; entry N is the top-left position where character N
    // starts. Populated from Layout's PaintableFragments when available — which gives correct positions for wrapping
    // text; falls back to single-line font-metrics approximation otherwise.
    Vector<Gfx::IntPoint> character_offsets;

    // For text leaf nodes, character offsets where a new line begins in the rendered layout. Entry 0 is always 0
    // (the first line starts at the first character). Populated from Layout's PaintableFragments.
    Vector<i32> line_break_character_offsets;

    // Per-line height in CSS pixels (index matches line_break_character_offsets). Used to compute vertical extent of
    // per-character rectangles for wrapping text.
    Vector<i32> line_heights;
};

}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::AccessibilityNodeData const&);

template<>
WEBVIEW_API ErrorOr<WebView::AccessibilityNodeData> decode(Decoder&);

}
