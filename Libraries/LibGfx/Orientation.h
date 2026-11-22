/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Gfx {

enum class Orientation {
    Horizontal,
    Vertical
};

// How a glyph run is set along its line: as horizontal text, or turned a quarter turn clockwise to run down a vertical
// line, or a quarter turn counterclockwise to run up one.
enum class GlyphRunOrientation {
    Horizontal,
    Vertical,
    VerticalCounterclockwise,
};

}

using Gfx::Orientation;
