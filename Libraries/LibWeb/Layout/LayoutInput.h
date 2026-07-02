/*
 * Copyright (c) 2026, Ladybird contributors
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
    explicit LayoutInput(AvailableSpace new_available_space, ContainingBlockConstraints new_containing_block_constraints = {})
        : available_space(move(new_available_space))
        , containing_block_constraints(move(new_containing_block_constraints))
    {
    }

    [[nodiscard]] LayoutInput with_available_space(AvailableSpace new_available_space) const
    {
        return LayoutInput { move(new_available_space), containing_block_constraints };
    }

    AvailableSpace const available_space;
    ContainingBlockConstraints const containing_block_constraints;
};

}
