/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

namespace Compositing {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, FontResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ImageFrameResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, DisplayListResourceId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CanvasId);
// Names the connection whose canvases a surface belongs to. Never reused, unlike connection IDs.
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CanvasSurfaceOwnerId);

}
