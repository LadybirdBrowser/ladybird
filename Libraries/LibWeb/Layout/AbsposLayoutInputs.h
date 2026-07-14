/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
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
    CSSPixelRect rect;
    AbsposAxisMode horizontal_axis_mode;
    AbsposAxisMode vertical_axis_mode;
    // Grid alignment for axes with auto CSS insets.
    // When set, the base method applies alignment-driven insets after sizing.
    Optional<Alignment> horizontal_alignment;
    Optional<Alignment> vertical_alignment;
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

    CSSPixelRect rect;
    Alignment horizontal_alignment { Alignment::Start };
    Alignment vertical_alignment { Alignment::Start };
    // Whether the alignments were derived from the box's own computed values (self-alignment
    // under a flex container does this); a saved copy of such a rect cannot be replayed after
    // a style change on the box itself.
    bool alignment_derives_from_own_computed_values { false };

    CSSPixelPoint aligned_position_for_box_with_size(CSSPixelSize const& size) const
    {
        CSSPixelPoint position = rect.location();
        if (horizontal_alignment == Alignment::Center)
            position.set_x(position.x() + (rect.width() - size.width()) / 2);
        else if (horizontal_alignment == Alignment::End)
            position.set_x(position.x() + rect.width() - size.width());

        if (vertical_alignment == Alignment::Center)
            position.set_y(position.y() + (rect.height() - size.height()) / 2);
        else if (vertical_alignment == Alignment::End)
            position.set_y(position.y() + rect.height() - size.height());

        return position;
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
