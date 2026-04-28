/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/DistinctNumeric.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/ScrollFrame.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class ScrollStateSnapshot;

AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, VisualContextIndex);

struct ScrollData {
    ScrollFrameIndex scroll_frame_index;
    bool is_sticky;
};

struct ClipData {
    DevicePixelRect rect;
    CornerRadii corner_radii;

    ClipData(DevicePixelRect r, CornerRadii radii)
        : rect(r)
        , corner_radii(radii)
    {
    }

    bool contains(DevicePixelPoint point) const;
};

struct TransformData {
    Gfx::FloatMatrix4x4 matrix;
    Gfx::FloatPoint origin;
};

struct PerspectiveData {
    Gfx::FloatMatrix4x4 matrix;
};

struct ClipPathData {
    Gfx::Path path;
    DevicePixelRect bounding_rect;
    Gfx::WindingRule fill_rule;
};

struct EffectsData {
    float opacity { 1.0f };
    Gfx::CompositingAndBlendingOperator blend_mode { Gfx::CompositingAndBlendingOperator::Normal };
    Optional<Gfx::Filter> gfx_filter;

    bool needs_layer() const
    {
        return opacity < 1.0f
            || blend_mode != Gfx::CompositingAndBlendingOperator::Normal
            || gfx_filter.has_value();
    }
};

using VisualContextData = Variant<ScrollData, ClipData, TransformData, PerspectiveData, ClipPathData, EffectsData>;

struct AccumulatedVisualContextNode {
    VisualContextData data;
    VisualContextIndex parent_index {};
    size_t depth { 0 };
    bool has_empty_effective_clip { false };
};

class AccumulatedVisualContextTree : public AtomicRefCounted<AccumulatedVisualContextTree> {
public:
    static NonnullRefPtr<AccumulatedVisualContextTree> create();

    VisualContextIndex append(VisualContextData data, VisualContextIndex parent_index);

    AccumulatedVisualContextNode const& node_at(VisualContextIndex index) const { return m_nodes[index.value()]; }

    VisualContextIndex find_common_ancestor(VisualContextIndex a, VisualContextIndex b) const;
    Optional<Gfx::FloatPoint> transform_point_for_hit_test(VisualContextIndex, Gfx::FloatPoint, ScrollStateSnapshot const&) const;
    Gfx::FloatPoint inverse_transform_point(VisualContextIndex, Gfx::FloatPoint) const;
    Gfx::FloatRect transform_rect_to_viewport(VisualContextIndex, Gfx::FloatRect const&, ScrollStateSnapshot const&) const;
    void dump(VisualContextIndex, StringBuilder&) const;

    bool has_empty_effective_clip(VisualContextIndex i) const { return m_nodes[i.value()].has_empty_effective_clip; }

private:
    AccumulatedVisualContextTree() = default;

    Vector<size_t, 8> build_ancestor_chain(VisualContextIndex index) const;

    Vector<AccumulatedVisualContextNode> m_nodes;
};

}
