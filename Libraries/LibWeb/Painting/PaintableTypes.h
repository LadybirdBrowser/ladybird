/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/WeakPtr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/BoxModelMetrics.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

enum class PaintPhase {
    Background,
    Border,
    TableCollapsedBorder,
    Foreground,
    Outline,
    Overlay,
};

enum class SelectionState : u8 {
    None,
    Start,
    End,
    StartAndEnd,
    Full,
};

// One record per line box of a block container with inline children, including lines without
// fragments (e.g. blank lines between consecutive forced breaks in a textarea). The rect is
// relative to the containing block's content-box origin, and fragment_count only counts
// committed fragments (fully truncated ones are never committed).
struct LineRecord {
    CSSPixelRect rect;
    u32 fragment_count { 0 };
};

// One line's slice of an inline box (InlineNode or inline-flow ListItemBox) fragmented across
// lines; box edges cut at a line boundary are absent, per box-decoration-break: slice. Fragment
// indexes point into the containing block paintable's fragment list, and border_box_rect is
// relative to that block's content-box origin. The committed piece list is ordered by (line,
// nesting depth), so outer boxes' pieces precede nested ones on the same line.
struct InlineBoxPiece {
    enum class Edge : u8 {
        Top = 1 << 0,
        Right = 1 << 1,
        Bottom = 1 << 2,
        Left = 1 << 3,
    };

    WeakPtr<Layout::Node const> node;
    u32 first_fragment_index { 0 };
    u32 fragment_count { 0 };
    CSSPixelRect border_box_rect;
    u8 present_edges { 0 };
    bool is_geometry_only_placeholder { false };

    bool has_edge(Edge edge) const { return present_edges & to_underlying(edge); }

    CSSPixelRect shrunken_by_present_edges(CSSPixelRect rect, PixelBox const& side_widths) const
    {
        rect.shrink(
            has_edge(Edge::Top) ? side_widths.top : 0,
            has_edge(Edge::Right) ? side_widths.right : 0,
            has_edge(Edge::Bottom) ? side_widths.bottom : 0,
            has_edge(Edge::Left) ? side_widths.left : 0);
        return rect;
    }
};

}
