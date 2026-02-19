/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibWeb/Forward.h>

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

Gfx::CompositingAndBlendingOperator mix_blend_mode_to_compositing_and_blending_operator(CSS::MixBlendMode blend_mode);

}
