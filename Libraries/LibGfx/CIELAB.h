/*
 * Copyright (c) 2023, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Gfx {

// https://en.wikipedia.org/wiki/CIELAB_color_space
struct CIELAB {
    float L; // L*
    float a; // a*
    float b; // b*
};

}
