/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Variant.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

struct ScrollData {
    size_t scroll_frame_id;
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

class AccumulatedVisualContext : public AtomicRefCounted<AccumulatedVisualContext> {
public:
    static NonnullRefPtr<AccumulatedVisualContext> create(size_t id, VisualContextData data, RefPtr<AccumulatedVisualContext const> parent);

    VisualContextData const& data() const { return m_data; }
    RefPtr<AccumulatedVisualContext const> parent() const { return m_parent; }

    bool is_effect() const { return m_data.has<EffectsData>(); }
    bool has_empty_effective_clip() const { return m_has_empty_effective_clip; }

    size_t depth() const { return m_depth; }
    size_t id() const { return m_id; }

    void dump(StringBuilder&) const;

    Optional<Gfx::FloatPoint> transform_point_for_hit_test(Gfx::FloatPoint, ReadonlySpan<Gfx::FloatPoint> scroll_offsets) const;
    Gfx::FloatPoint inverse_transform_point(Gfx::FloatPoint) const;
    Gfx::FloatRect transform_rect_to_viewport(Gfx::FloatRect const&, ReadonlySpan<Gfx::FloatPoint> scroll_offsets) const;

private:
    AccumulatedVisualContext(size_t id, VisualContextData data, RefPtr<AccumulatedVisualContext const> parent)
        : m_data(move(data))
        , m_parent(move(parent))
        , m_depth(m_parent ? m_parent->depth() + 1 : 1)
        , m_id(id)
    {
        if (m_parent && m_parent->has_empty_effective_clip()) {
            m_has_empty_effective_clip = true;
        } else if (m_data.has<ClipData>()) {
            m_has_empty_effective_clip = m_data.get<ClipData>().rect.is_empty();
        } else if (m_data.has<ClipPathData>()) {
            m_has_empty_effective_clip = m_data.get<ClipPathData>().path.bounding_box().is_empty();
        }
    }

    VisualContextData m_data;
    RefPtr<AccumulatedVisualContext const> m_parent;
    size_t m_depth;
    size_t m_id;
    bool m_has_empty_effective_clip { false };
};

}
