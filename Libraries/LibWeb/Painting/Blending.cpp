/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Blending.h"
#include <LibWeb/CSS/Enums.h>

namespace Web::Painting {

Gfx::CompositingAndBlendingOperator mix_blend_mode_to_compositing_and_blending_operator(CSS::MixBlendMode blend_mode)
{
    switch (blend_mode) {
#undef __ENUMERATE
#define __ENUMERATE(blend_mode)         \
    case CSS::MixBlendMode::blend_mode: \
        return Gfx::CompositingAndBlendingOperator::blend_mode;
        ENUMERATE_MIX_BLEND_MODES(__ENUMERATE)
#undef __ENUMERATE
    default:
        VERIFY_NOT_REACHED();
    }
}

}
