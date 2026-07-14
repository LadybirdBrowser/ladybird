/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Layout/AvailableSpace.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

struct ContainingBlockConstraints {
    Optional<CSSPixels> percentage_basis_width;
    Optional<CSSPixels> percentage_basis_height;
    Optional<CSSPixels> quirks_mode_percentage_basis_height;
};

struct LayoutInput {
    explicit LayoutInput(AvailableSpace new_available_space, ContainingBlockConstraints new_containing_block_constraints = {}, Optional<CSSPixelPoint> new_content_box_position_in_bfc_root = {}, Optional<CSSPixels> new_table_grid_min_border_box_height = {})
        : available_space(move(new_available_space))
        , containing_block_constraints(move(new_containing_block_constraints))
        , content_box_position_in_bfc_root(new_content_box_position_in_bfc_root)
        , table_grid_min_border_box_height(new_table_grid_min_border_box_height)
    {
    }

    [[nodiscard]] LayoutInput for_child_formatting_context(AvailableSpace new_available_space) const
    {
        return LayoutInput { move(new_available_space), containing_block_constraints };
    }

    [[nodiscard]] LayoutInput with_content_box_position_in_bfc_root(Optional<CSSPixelPoint> position) const
    {
        return LayoutInput { available_space, containing_block_constraints, position, table_grid_min_border_box_height };
    }

    [[nodiscard]] LayoutInput with_table_grid_min_border_box_height(CSSPixels height) const
    {
        return LayoutInput { available_space, containing_block_constraints, content_box_position_in_bfc_root, height };
    }

    AvailableSpace const available_space;
    ContainingBlockConstraints const containing_block_constraints;

    Optional<CSSPixelPoint> const content_box_position_in_bfc_root;
    Optional<CSSPixels> const table_grid_min_border_box_height;
};

}
