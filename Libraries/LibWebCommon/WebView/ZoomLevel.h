/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>

namespace WebView {

static constexpr double ZOOM_MIN_LEVEL = 0.3;
static constexpr double ZOOM_MAX_LEVEL = 5.0;

// The zoom level a tab takes when asked for `zoom_level`.
constexpr double clamp_zoom_level(double zoom_level)
{
    return clamp(zoom_level, ZOOM_MIN_LEVEL, ZOOM_MAX_LEVEL);
}

}
