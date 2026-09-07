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
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, ClipNodeIndex);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u32, EffectNodeIndex);

// Spatial node 0 is always the visual viewport transform, so index 0 doubles as "no node" for
// references to scroll nodes, which can never sit at the root.
static constexpr SpatialNodeIndex VISUAL_VIEWPORT_NODE_INDEX { 0 };
static constexpr ClipNodeIndex NO_CLIP_NODE { NumericLimits<u32>::max() };
static constexpr EffectNodeIndex NO_EFFECT_NODE { NumericLimits<u32>::max() };

struct ContextRef {
    SpatialNodeIndex spatial { VISUAL_VIEWPORT_NODE_INDEX };
    ClipNodeIndex clip { NO_CLIP_NODE };
    EffectNodeIndex effect { NO_EFFECT_NODE };

    bool operator==(ContextRef const&) const = default;
};

static_assert(sizeof(ContextRef) == 12);

}

template<>
struct AK::Formatter<Web::Painting::SpatialNodeIndex> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Painting::SpatialNodeIndex index)
    {
        return Formatter<FormatString>::format(builder, "s{}"sv, index.value());
    }
};

template<>
struct AK::Formatter<Web::Painting::ClipNodeIndex> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Painting::ClipNodeIndex index)
    {
        return Formatter<FormatString>::format(builder, "c{}"sv, index.value());
    }
};

template<>
struct AK::Formatter<Web::Painting::EffectNodeIndex> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Painting::EffectNodeIndex index)
    {
        return Formatter<FormatString>::format(builder, "e{}"sv, index.value());
    }
};

template<>
struct AK::Formatter<Web::Painting::ContextRef> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Painting::ContextRef context)
    {
        TRY(Formatter<FormatString>::format(builder, "{}"sv, context.spatial));
        if (context.clip != Web::Painting::NO_CLIP_NODE)
            TRY(Formatter<FormatString>::format(builder, "/{}"sv, context.clip));
        if (context.effect != Web::Painting::NO_EFFECT_NODE)
            TRY(Formatter<FormatString>::format(builder, "/{}"sv, context.effect));
        return {};
    }
};
