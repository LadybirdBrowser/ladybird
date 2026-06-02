/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/WindingRule.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/ScrollFrame.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class ScrollStateSnapshot;

AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, VisualContextIndex);

static constexpr VisualContextIndex VISUAL_VIEWPORT_NODE_INDEX { 0 };

struct ScrollData {
    ScrollFrameIndex scroll_frame_index;
    bool is_sticky;
};

struct ClipData {
    DevicePixelRect rect;
    Gfx::CornerRadii corner_radii;

    ClipData(DevicePixelRect r, Gfx::CornerRadii radii)
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

// Negates a scroll frame's offset during display list replay. Used to keep fixed backgrounds stationary relative to
// the viewport regardless of scroll position.
struct ScrollCompensation {
    ScrollFrameIndex scroll_frame_index;
};

using VisualContextData = Variant<ScrollData, ClipData, TransformData, PerspectiveData, ClipPathData, EffectsData, ScrollCompensation>;

struct AccumulatedVisualContextNode {
    VisualContextData data;
    VisualContextIndex parent_index {};
    size_t depth { 0 };
    bool has_empty_effective_clip { false };
};

class AccumulatedVisualContextTree {
public:
    static AccumulatedVisualContextTree create();
    static AccumulatedVisualContextTree create(TransformData visual_viewport_transform);

    AccumulatedVisualContextTree(AccumulatedVisualContextTree const&) = default;
    AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree const&) = default;
    AccumulatedVisualContextTree(AccumulatedVisualContextTree&&) = default;
    AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree&&) = default;
    ~AccumulatedVisualContextTree() = default;

    u64 version() const { return m_version; }

    VisualContextIndex append(VisualContextData data, VisualContextIndex parent_index);
    void set_visual_viewport_transform(TransformData);

    AccumulatedVisualContextNode const& node_at(VisualContextIndex index) const { return m_nodes[index.value()]; }
    ReadonlySpan<AccumulatedVisualContextNode> nodes() const { return m_nodes.span(); }

    VisualContextIndex find_common_ancestor(VisualContextIndex a, VisualContextIndex b) const;
    Optional<Gfx::FloatPoint> transform_point_for_hit_test(VisualContextIndex, Gfx::FloatPoint, ScrollStateSnapshot const&) const;
    Gfx::FloatPoint inverse_transform_point(VisualContextIndex, Gfx::FloatPoint) const;
    Gfx::FloatRect transform_rect_to_viewport(VisualContextIndex, Gfx::FloatRect const&, ScrollStateSnapshot const&) const;
    void dump(VisualContextIndex, StringBuilder&) const;

    bool has_empty_effective_clip(VisualContextIndex i) const { return m_nodes[i.value()].has_empty_effective_clip; }

private:
    AccumulatedVisualContextTree(u64 version, Vector<AccumulatedVisualContextNode>&& nodes)
        : m_version(version)
        , m_nodes(move(nodes))
    {
    }

    Vector<size_t, 8> build_ancestor_chain(VisualContextIndex index) const;

    u64 m_version { 0 };
    Vector<AccumulatedVisualContextNode> m_nodes;

    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ScrollData const&);
template<>
WEB_API ErrorOr<Web::Painting::ScrollData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ClipData const&);
template<>
WEB_API ErrorOr<Web::Painting::ClipData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::TransformData const&);
template<>
WEB_API ErrorOr<Web::Painting::TransformData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::PerspectiveData const&);
template<>
WEB_API ErrorOr<Web::Painting::PerspectiveData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ClipPathData const&);
template<>
WEB_API ErrorOr<Web::Painting::ClipPathData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::EffectsData const&);
template<>
WEB_API ErrorOr<Web::Painting::EffectsData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ScrollCompensation const&);
template<>
WEB_API ErrorOr<Web::Painting::ScrollCompensation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::AccumulatedVisualContextNode const&);
template<>
WEB_API ErrorOr<Web::Painting::AccumulatedVisualContextNode> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::AccumulatedVisualContextTree const&);
template<>
WEB_API ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder&);

}
