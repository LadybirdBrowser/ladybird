/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

enum class PaintCommandCacheMode : u8 {
    ReadOnly,
    ReadWrite,
};

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

// Used grid track data captured at layout time as plain values; getComputedStyle
// reflection mints style values from it on demand.
struct UsedGridTrackList {
    bool is_subgrid { false };
    // One entry per grid line (one more line than there are tracks, unless subgrid);
    // a line's name list may be empty.
    Vector<CSS::GridLineNames> lines;
    Vector<CSSPixels> track_sizes;
};

struct TextDecorationStyle {
    Vector<CSS::TextDecorationLine> line;
    CSS::TextDecorationStyle style;
    Color color;
};

struct SelectionStyle {
    Color background_color;
    Optional<Color> text_color {};
    Optional<Vector<ShadowData>> text_shadow {};
    Optional<TextDecorationStyle> text_decoration {};

    bool has_styling() const
    {
        return background_color.alpha() > 0 || text_color.has_value() || text_shadow.has_value() || text_decoration.has_value();
    }
};

struct OverflowData {
    CSSPixelRect scrollable_overflow_rect;
    bool has_scrollable_overflow { false };
};

}
