/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Variant.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Path.h>
#include <LibGfx/WindingRule.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

struct ClipRect {
    CSSPixelRect rect;
    BorderRadiiData corner_radii;
};

struct ScrollData {
    size_t scroll_frame_id;
    bool is_sticky;
};

struct ClipData {
    CSSPixelRect rect;
    BorderRadiiData corner_radii;

    explicit ClipData(ClipRect const& clip_rect)
        : rect(clip_rect.rect)
        , corner_radii(clip_rect.corner_radii)
    {
    }

    ClipData(CSSPixelRect r, BorderRadiiData radii)
        : rect(r)
        , corner_radii(radii)
    {
    }
};

struct TransformData {
    Gfx::FloatMatrix4x4 matrix;
    CSSPixelPoint origin;
};

struct PerspectiveData {
    Gfx::FloatMatrix4x4 matrix;
};

struct ClipPathData {
    Gfx::Path path;
    CSSPixelRect bounding_rect;
    Gfx::WindingRule fill_rule;
};

using VisualContextData = Variant<ScrollData, ClipData, TransformData, PerspectiveData, ClipPathData>;

class AccumulatedVisualContext : public AtomicRefCounted<AccumulatedVisualContext> {
public:
    static NonnullRefPtr<AccumulatedVisualContext> create(size_t id, VisualContextData data, RefPtr<AccumulatedVisualContext const> parent);

    VisualContextData const& data() const { return m_data; }
    RefPtr<AccumulatedVisualContext const> parent() const { return m_parent; }

    bool is_scroll() const { return m_data.has<ScrollData>(); }
    bool is_clip() const { return m_data.has<ClipData>(); }
    bool is_transform() const { return m_data.has<TransformData>(); }
    bool is_perspective() const { return m_data.has<PerspectiveData>(); }
    bool is_clip_path() const { return m_data.has<ClipPathData>(); }

    size_t depth() const { return m_depth; }
    size_t id() const { return m_id; }

    void dump(StringBuilder&) const;

    Optional<CSSPixelPoint> transform_point_for_hit_test(CSSPixelPoint screen_point, ScrollStateSnapshot const& scroll_state) const;
    CSSPixelRect transform_rect_to_viewport(CSSPixelRect const&, ScrollStateSnapshot const&) const;

private:
    AccumulatedVisualContext(size_t id, VisualContextData data, RefPtr<AccumulatedVisualContext const> parent)
        : m_data(move(data))
        , m_parent(move(parent))
        , m_depth(m_parent ? m_parent->depth() + 1 : 1)
        , m_id(id)
    {
    }

    VisualContextData m_data;
    RefPtr<AccumulatedVisualContext const> m_parent;
    size_t m_depth;
    size_t m_id;
};

}
