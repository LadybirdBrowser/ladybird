/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Forward.h>

class SkFont;
class SkSurfaceProps;

namespace Compositor {

enum class FontRasterizer {
    // The font manager of the platform: CoreText on macOS, DirectWrite on Windows, and FreeType elsewhere.
    Platform,
    FreeType,
};

// Call this before any text is drawn.
void set_font_rasterizer(FontRasterizer);

// Keep the font rendering settings used by the test baselines.
void use_test_font_rendering();

SkSurfaceProps skia_text_surface_properties(u32 flags, bool allows_lcd_text);

// The Skia font that draws the glyphs of a font at a scale. Empty if Skia cannot load the font's typeface; the caller
// then draws nothing.
Optional<SkFont> skia_font(Gfx::Font const&, float scale, bool requests_lcd_text = false);

}
