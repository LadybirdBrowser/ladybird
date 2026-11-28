/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/NeverDestroyed.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>

namespace WebView {

// A part of a node's data that most nodes leave at its defaults, or that many nodes hold the same value of. A node holds
// it out of line, and only when it differs from the defaults, so a node without it costs one null pointer. Copies of a
// node share it until one of them changes it.
// NB: The reference count isn't atomic, so the nodes that share a value have to stay on one thread.
template<typename T>
class AccessibilityShared {
public:
    T const& operator*() const { return m_holder ? m_holder->value : default_value(); }
    T const* operator->() const { return &**this; }

    // Whether this holds no value of its own, which reads as the defaults.
    bool is_default() const { return !m_holder; }
    // How many nodes share the value held, or 0 when there's none.
    unsigned ref_count() const { return m_holder ? m_holder->ref_count() : 0; }

    // The value to change, which is no longer shared with any other node.
    T& ensure()
    {
        if (!m_holder)
            m_holder = adopt_ref(*new Holder);
        else if (m_holder->ref_count() > 1)
            m_holder = adopt_ref(*new Holder(m_holder->value));
        return m_holder->value;
    }
    void set(T value) { m_holder = adopt_ref(*new Holder(move(value))); }
    void clear() { m_holder = nullptr; }

    bool operator==(AccessibilityShared const& other) const { return m_holder == other.m_holder || **this == *other; }

private:
    struct Holder : public RefCounted<Holder> {
        Holder() = default;
        explicit Holder(T value)
            : value(move(value))
        {
        }
        T value;
    };

    static T const& default_value()
    {
        static NeverDestroyed<T const> value {};
        return *value;
    }

    RefPtr<Holder> m_holder;
};

// What some elements' nodes hold beyond the fields every node has.
struct WEBCOMMON_API AccessibilityNodeDetails {
    String description;
    String value;

    // For link elements, the href URL. Empty for non-links.
    String url;

    // For the document root, the IETF BCP 47 language tag from the html[lang] attribute. Empty otherwise.
    String language;

    String live; // "assertive", "polite", "off", or empty

    // Keyboard shortcut (accesskey attribute) for AtkAction::get_keybinding. Empty if none.
    String keybinding;

    i32 column_span { 1 };
    i32 row_span { 1 };

    // For AtkTableCell: 0-based column index of this cell within its row, col_span summed. -1 for non-cells. (The row
    // index isn't serialized: the bridges derive it from the row's position in the tree.)
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

    // AtkValue fields for sliders, progress bars, meters, spinbuttons. Empty when not applicable.
    Optional<double> value_numeric;
    Optional<double> value_minimum;
    Optional<double> value_maximum;
    Optional<double> value_step;

    bool operator==(AccessibilityNodeDetails const&) const = default;
};

// Text-formatting attributes for text leaf nodes, read from the parent element's computed style. Stored as strings in the
// format expected by AT-SPI2. Many text leaves have the same, so they share one.
struct WEBCOMMON_API AccessibilityTextStyle {
    String font_family;
    String font_size;   // CSS pixel size as a decimal string; e.g., "16"
    String font_weight; // Numeric weight as decimal; e.g., "400" / "700"
    String font_style;  // "normal" / "italic" / "oblique"
    String color;       // e.g., "#000000"
    String background_color;
    String text_decoration; // "none" / "underline" / "line-through" / space-separated combinations

    unsigned hash() const;
    bool operator==(AccessibilityTextStyle const&) const = default;
};

// Where each character of a text leaf node renders. Populated from the text's rendered visual lines when it has any,
// which gives correct positions for wrapping text; falls back to a single-line font-metrics approximation otherwise.
struct WEBCOMMON_API AccessibilityTextGeometry {
    // Builds the geometry from each character's (x, y) offset in CSS pixels from the top-left of the node's bounds, and
    // the character offset, top and height of each line the text renders on.
    static AccessibilityTextGeometry create(ReadonlySpan<Gfx::IntPoint> character_offsets, ReadonlySpan<i32> line_break_character_offsets, ReadonlySpan<i32> line_tops, ReadonlySpan<i32> line_heights);

    // One entry per character: the top-left position where character N starts.
    size_t character_count() const { return layout().character_count; }
    Gfx::IntPoint character_offset(size_t index) const;

    // The line that the character at `index` is on: the last one that starts at or before it.
    size_t line_of(size_t index) const;

    // Character offsets where a new line begins in the rendered layout. Entry 0 is always 0 (the first line starts at the
    // first character).
    ReadonlySpan<i32> line_break_character_offsets() const { return section(0); }
    // Per-line top and height in CSS pixels (index matches line_break_character_offsets). The heights give the vertical
    // extent of per-character rectangles for wrapping text.
    ReadonlySpan<i32> line_tops() const { return section(1); }
    ReadonlySpan<i32> line_heights() const { return section(2); }

    // OPTIMIZATION: Most text leaves are a few words on one line, so all of it is in one allocation: the line count,
    //               whether every character's y follows (1) or each character's y is the top of its line (0), then
    //               each line's start, top and height, then each character's x, then, when they follow, each
    //               character's y.
    Vector<i32> data;

    bool operator==(AccessibilityTextGeometry const&) const = default;

private:
    static constexpr size_t HEADER_SIZE = 2;
    struct Layout {
        size_t line_count { 0 };
        size_t character_count { 0 };
        bool has_character_y { false };
    };
    // How the data is laid out, or nothing in it when it doesn't fit that layout (it comes from WebContent).
    Layout layout() const;
    // The line starts (0), tops (1) or heights (2).
    ReadonlySpan<i32> section(size_t index) const;
};

struct WEBCOMMON_API AccessibilityNodeData {
    AK_ALLOC_WITH_KMALLOC;

    i64 id { 0 };
    i64 parent_id { -1 };
    Vector<i64> child_ids;

    // OPTIMIZATION: Interned, since a handful of roles is all the nodes of a tree have between them.
    FlyString role;
    String name;

    // The node's box in CSS pixels, relative to the viewport (getBoundingClientRect). A text leaf's is the union of its
    // rendered lines, or its parent element's box when it renders none.
    Gfx::IntRect bounds;

    bool is_focused { false };
    bool is_disabled { false };
    i32 heading_level { 0 };      // a heading's level
    i32 hierarchical_level { 0 }; // aria-level on a treeitem, row or listitem: its depth

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

    enum class PressedState : u8 {
        NotApplicable,
        NotPressed,
        Pressed,
        Mixed,
    };

    bool is_editable { false };         // Text input, textarea, contenteditable
    bool is_multi_line { false };       // textarea element or contenteditable block
    bool is_password { false };         // input type=password; value then carries the mask the field renders
    bool is_read_only { false };        // "readonly" attribute or aria-readonly=true
    bool is_required { false };         // "required" attribute or aria-required=true
    bool is_invalid { false };          // aria-invalid=true
    bool is_multi_selectable { false }; // select[multiple] or aria-multiselectable=true
    PressedState pressed_state { PressedState::NotApplicable };
    bool is_visited { false }; // Visited links

    // Caret position within this node's text, as a Unicode character offset. -1 if the caret is not in this node.
    // Populated from the DOM Selection for the node containing the selection focus (or the collapsed-selection caret —
    // that is, no text selected and cursor positioned here).
    i32 caret_offset { -1 };

    // Text-selection range within this node's text, as Unicode character offsets. Both are -1 if there is no selection
    // in this node. When set, [selection_start, selection_end) describes the selected range.
    i32 selection_start { -1 };
    i32 selection_end { -1 };

    // The fields only some nodes have: see AccessibilityNodeDetails.
    AccessibilityShared<AccessibilityNodeDetails> details;

    // For text leaf nodes, their formatting. Empty on non-text-leaf nodes.
    AccessibilityShared<AccessibilityTextStyle> text_style;

    // For text leaf nodes, where each character renders. Empty on non-text-leaf nodes.
    AccessibilityShared<AccessibilityTextGeometry> text_geometry;

    bool operator==(AccessibilityNodeData const&) const = default;
};

// A change to an accessibility tree the receiver already holds: the complete data of every node that's new or whose data
// changed (a node whose children changed among them, with its new child IDs), in tree order, and the IDs of the nodes no
// longer in the tree. A node that moves keeps its ID: Its old and new parents are both among the changed nodes. The order
// decides which of several nodes inserted into a live region the region announces: the first.
struct WEBCOMMON_API AccessibilityTreeDelta {
    Vector<AccessibilityNodeData> changed_nodes;
    Vector<i64> removed_ids;
};

// A whole accessibility tree or a change to one, encoded into shared memory by WebContent and decoded only by the UI that
// consumes it. The hub between them passes it along as it is.
// OPTIMIZATION: A whole tree can have hundreds of thousands of nodes, which the hub would otherwise build, encode again,
//               and free, keeping the memory that took.
struct WEBCOMMON_API EncodedAccessibilityTree {
    static ErrorOr<EncodedAccessibilityTree> encode(Vector<AccessibilityNodeData> const&);
    ErrorOr<Vector<AccessibilityNodeData>> decode() const;

    Core::AnonymousBuffer buffer;
};

struct WEBCOMMON_API EncodedAccessibilityTreeDelta {
    static ErrorOr<EncodedAccessibilityTreeDelta> encode(AccessibilityTreeDelta const&);
    ErrorOr<AccessibilityTreeDelta> decode() const;

    Core::AnonymousBuffer buffer;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::AccessibilityNodeData const&);

template<>
WEBCOMMON_API ErrorOr<WebView::AccessibilityNodeData> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::EncodedAccessibilityTree const&);

template<>
WEBCOMMON_API ErrorOr<WebView::EncodedAccessibilityTree> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::EncodedAccessibilityTreeDelta const&);

template<>
WEBCOMMON_API ErrorOr<WebView::EncodedAccessibilityTreeDelta> decode(Decoder&);

}

#include <LibWebCommon/WebView/AccessibilityNodeDataCodecs.h>
