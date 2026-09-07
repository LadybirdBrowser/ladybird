/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/ContextRef.h>
#include <LibWeb/Painting/DisplayListCommand.h>

namespace Web::Painting {

// Builds a visual context tree node by node in Rust for unit tests, which otherwise only ever
// receive trees the layout arena built.
class WEB_API VisualContextTreeTestBuilder {
    AK_MAKE_NONCOPYABLE(VisualContextTreeTestBuilder);
    AK_MAKE_NONMOVABLE(VisualContextTreeTestBuilder);

public:
    struct StickyConstraints {
        SpatialNodeIndex scroller;
        Optional<SpatialNodeIndex> parent_sticky;
        Gfx::FloatPoint position_relative_to_scroller;
        Gfx::FloatSize border_box_size;
        Gfx::FloatSize scrollport_size;
        Gfx::FloatRect containing_block_region;
        bool needs_parent_offset_adjustment { false };
        Optional<float> inset_top;
        Optional<float> inset_right;
        Optional<float> inset_bottom;
        Optional<float> inset_left;
    };

    VisualContextTreeTestBuilder();
    ~VisualContextTreeTestBuilder();

    SpatialNodeIndex append_transform(SpatialNodeIndex parent, Gfx::FloatMatrix4x4 const&, Gfx::FloatPoint origin = {});
    SpatialNodeIndex append_scroll(SpatialNodeIndex parent);
    SpatialNodeIndex append_sticky(SpatialNodeIndex parent, StickyConstraints const&);
    ClipNodeIndex append_clip(ClipNodeIndex parent, SpatialNodeIndex spatial, Gfx::FloatRect, Gfx::CornerRadii = {}, ClipMode = ClipMode::Intersect);
    ClipNodeIndex append_clip_path(ClipNodeIndex parent, SpatialNodeIndex spatial, Gfx::Path const&, Gfx::IntRect bounding_rect, Gfx::WindingRule);
    EffectNodeIndex append_effects(EffectNodeIndex parent, SpatialNodeIndex spatial, ClipNodeIndex output_clip = NO_CLIP_NODE, float opacity = 1.0f, Gfx::CompositingAndBlendingOperator = Gfx::CompositingAndBlendingOperator::Normal);
    EffectNodeIndex append_background_color_animation(EffectNodeIndex parent, SpatialNodeIndex spatial, ClipNodeIndex output_clip = NO_CLIP_NODE);
    // The frame a context names for recording under the given clip and effect.

    AccumulatedVisualContextTree finish();
    AccumulatedVisualContextTree finish_with_structural_epoch(u64 structural_epoch);

private:
    void* m_builder { nullptr };
};

}
