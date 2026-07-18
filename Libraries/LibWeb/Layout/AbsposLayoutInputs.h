/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Layout/LogicalGeometry.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

enum class Alignment {
    Baseline,
    Center,
    End,
    Normal,
    Safe,
    SelfEnd,
    SelfStart,
    SpaceAround,
    SpaceBetween,
    SpaceEvenly,
    Start,
    Stretch,
    Unsafe,
};

enum class AbsposAxisMode {
    // Both insets auto: offset = static_position + margin
    StaticPosition,
    // At least one explicit inset: offset = rect.origin + inset + margin
    InsetFromRect,
};

struct AbsposContainingBlockInfo {
    // Containing block rect in CB Box's content-edge coordinates.
    LogicalRect rect;
    AbsposAxisMode inline_axis_mode;
    AbsposAxisMode block_axis_mode;
    // Grid alignment for axes with auto CSS insets.
    // When set, the base method applies alignment-driven insets after sizing.
    Optional<Alignment> inline_alignment;
    Optional<Alignment> block_alignment;
    // Whether the rect, alignments or axis modes were derived from the box's own computed
    // values (grid placement does this); a saved copy of such inputs cannot be replayed after
    // a style change on the box itself, and its axis modes must not be recomputed at replay.
    bool derives_from_own_computed_values { false };
};

// https://www.w3.org/TR/css-position-3/#static-position-rectangle
struct StaticPositionRect {
    enum class Alignment {
        Start,
        Center,
        End,
    };

    LogicalRect rect;
    Alignment inline_alignment { Alignment::Start };
    Alignment block_alignment { Alignment::Start };
    // Whether the alignments were derived from the box's own computed values (self-alignment
    // under a flex container does this); a saved copy of such a rect cannot be replayed after
    // a style change on the box itself.
    bool alignment_derives_from_own_computed_values { false };

    LogicalOffset aligned_offset_for_box_with_size(LogicalSize const& size) const
    {
        auto offset = rect.offset;
        if (inline_alignment == Alignment::Center)
            offset.inline_offset += (rect.size.inline_size - size.inline_size) / 2;
        else if (inline_alignment == Alignment::End)
            offset.inline_offset += rect.size.inline_size - size.inline_size;

        if (block_alignment == Alignment::Center)
            offset.block_offset += (rect.size.block_size - size.block_size) / 2;
        else if (block_alignment == Alignment::End)
            offset.block_offset += rect.size.block_size - size.block_size;

        return offset;
    }
};

// Everything the absolutely positioned element layout algorithm consumes from outside the
// element's own subtree, as plain values resolved by the ancestor formatting context. A partial
// relayout can replay the algorithm from a saved copy without re-running the ancestor's layout.
struct AbsposLayoutInputs {
    StaticPositionRect static_position_rect;
    AbsposContainingBlockInfo containing_block_info;
};

}
