/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>

namespace Web::Painting {

AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, VisualContextIndex);

// Node 0 is always the visual viewport transform node, so index 0 doubles as "no node" for
// references to scroll nodes, which can never sit at the root.
static constexpr VisualContextIndex VISUAL_VIEWPORT_NODE_INDEX { 0 };

}
