/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Format.h>
#include <AK/NumericLimits.h>

namespace Web::Painting {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, SpatialNodeIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, FrameNodeIndex);

// Spatial node 0 is always the visual viewport transform, so index 0 doubles as "no node" for
// references to scroll nodes, which can never sit at the root.
static constexpr SpatialNodeIndex VISUAL_VIEWPORT_NODE_INDEX { 0 };
static constexpr FrameNodeIndex NO_FRAME_NODE { NumericLimits<u32>::max() };

struct ContextRef {
    SpatialNodeIndex spatial { VISUAL_VIEWPORT_NODE_INDEX };
    FrameNodeIndex frame { NO_FRAME_NODE };

    bool operator==(ContextRef const&) const = default;
};

static_assert(sizeof(ContextRef) == 8);

}

template<>
struct AK::Formatter<Web::Painting::SpatialNodeIndex> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Painting::SpatialNodeIndex index)
    {
        return Formatter<FormatString>::format(builder, "s{}"sv, index.value());
    }
};

template<>
struct AK::Formatter<Web::Painting::FrameNodeIndex> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Painting::FrameNodeIndex index)
    {
        return Formatter<FormatString>::format(builder, "f{}"sv, index.value());
    }
};

template<>
struct AK::Formatter<Web::Painting::ContextRef> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Painting::ContextRef context)
    {
        if (context.frame == Web::Painting::NO_FRAME_NODE)
            return Formatter<FormatString>::format(builder, "{}"sv, context.spatial);
        return Formatter<FormatString>::format(builder, "{}/{}"sv, context.spatial, context.frame);
    }
};
