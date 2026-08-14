/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>

namespace Web::Painting {

class Paintable;

AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, VisualContextIndex);

// Node 0 is always the visual viewport transform node, so index 0 doubles as "no node" for
// references to scroll nodes, which can never sit at the root.
static constexpr VisualContextIndex VISUAL_VIEWPORT_NODE_INDEX { 0 };

// Nested display list trees (masks, clips, patterns) assign visual context indices to paintables
// outside the main tree. The recording context carries the assignments, leaving the paintables'
// stored indices owned by the main tree for geometry APIs.
struct NestedVisualContextAssignments {
    struct PaintableIndices {
        VisualContextIndex own;
        VisualContextIndex for_descendants;
    };
    HashMap<Paintable const*, PaintableIndices> paintable_indices;
    HashMap<Paintable const*, Vector<VisualContextIndex>> mask_node_indices;
};

}
