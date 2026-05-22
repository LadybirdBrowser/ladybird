/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>

namespace Web::Painting {

struct GridInspectorOverlayOptions {
    Gfx::Color color { 76, 179, 212 };
    bool show_area_names { true };
    bool show_line_numbers { true };
    bool show_track_sizes { false };
    bool show_infinite_lines { false };
};

}
