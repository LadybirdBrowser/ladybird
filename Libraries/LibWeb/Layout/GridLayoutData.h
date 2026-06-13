/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

enum class GridTrackType : u8 {
    Explicit,
    Implicit,
};

enum class GridTrackState : u8 {
    Static,
    Repeat,
    Removed,
};

struct GridLayoutLine {
    Vector<String> names;
    CSSPixels start { 0 };
    CSSPixels breadth { 0 };
    GridTrackType type { GridTrackType::Implicit };
    u32 number { 0 };
    i32 negative_number { 0 };
};

struct GridLayoutTrack {
    CSSPixels start { 0 };
    CSSPixels breadth { 0 };
    GridTrackType type { GridTrackType::Implicit };
    GridTrackState state { GridTrackState::Static };
};

struct GridLayoutArea {
    String name;
    GridTrackType type { GridTrackType::Explicit };
    u32 row_start { 0 };
    u32 row_end { 0 };
    u32 column_start { 0 };
    u32 column_end { 0 };
};

struct GridLayoutDimension {
    Vector<GridLayoutLine> lines;
    Vector<GridLayoutTrack> tracks;
};

struct GridLayoutFragment {
    Vector<GridLayoutArea> areas;
    GridLayoutDimension columns;
    GridLayoutDimension rows;
};

struct GridLayoutData {
    AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::Layout);

    CSS::Direction direction { CSS::Direction::Ltr };
    CSS::WritingMode writing_mode { CSS::WritingMode::HorizontalTb };
    bool is_subgrid { false };
    Vector<GridLayoutFragment> fragments;
};

}
