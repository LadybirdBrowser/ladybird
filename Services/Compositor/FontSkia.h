/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Forward.h>

class SkFont;

namespace Compositor {

// Makes every font use normal hinting, so that text looks the same on every machine.
void force_font_hinting_for_testing();

// The Skia font that draws the glyphs of a font at a scale. Empty if Skia cannot load the font's typeface; the caller
// then draws nothing.
Optional<SkFont> skia_font(Gfx::Font const&, float scale);

}
