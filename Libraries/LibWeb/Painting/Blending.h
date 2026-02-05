/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::Painting {

#define ENUMERATE_MIX_BLEND_MODES(E) \
    E(Normal)                        \
    E(Multiply)                      \
    E(Screen)                        \
    E(Overlay)                       \
    E(Darken)                        \
    E(Lighten)                       \
    E(ColorDodge)                    \
    E(ColorBurn)                     \
    E(HardLight)                     \
    E(SoftLight)                     \
    E(Difference)                    \
    E(Exclusion)                     \
    E(Hue)                           \
    E(Saturation)                    \
    E(Color)                         \
    E(Luminosity)                    \
    E(PlusDarker)                    \
    E(PlusLighter)

static Gfx::CompositingAndBlendingOperator mix_blend_mode_to_compositing_and_blending_operator(CSS::MixBlendMode blend_mode)
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
