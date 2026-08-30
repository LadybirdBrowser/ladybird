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
#include <LibGfx/Forward.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/WindingRule.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class ScrollStateSnapshot;

// The node's own SpatialNodeIndex keys its entry in the scroll offset snapshot.
struct ScrollData { };

// A sticky box's shift, derived by resolve_sticky_offsets() from the scroller's snapshot entry and
// the parent sticky chain. Both references follow the containing block chain, which continues
// through fixed-position ancestors, so they need not be spatial ancestors of the node.
struct StickyData {
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

    bool operator==(StickyData const&) const = default;
};

enum class ClipMode : u8 {
    Intersect,
    Difference,
};

struct ClipData {
    Gfx::FloatRect rect;
    Gfx::CornerRadii corner_radii;
    ClipMode mode { ClipMode::Intersect };

    bool contains(Gfx::FloatPoint) const;
};

// Distinguishes the two producers of transform nodes so value-only updates can patch each from its
// own source: CSS transforms recompute from style, SVG viewport transforms from the viewport
// paintable's committed viewBox transform and box position.
enum class TransformDataRole : u8 {
    CssTransform,
    SvgViewportTransform,
};

struct TransformData {
    Gfx::FloatMatrix4x4 matrix;
    Gfx::FloatPoint origin;
    Optional<SpatialNodeIndex> sorting_context_root_index {};
    bool flattens_inherited_transform { false };
    TransformDataRole role { TransformDataRole::CssTransform };
    bool synthetic_plane { false };

    Gfx::FloatMatrix4x4 matrix_including_origin() const;
};

struct PerspectiveData {
    Gfx::FloatMatrix4x4 matrix;
    bool flattens_inherited_transform { false };
};

struct BackfaceVisibilityData {
    SpatialNodeIndex plane_root_index;
    bool flattens_inherited_transform { false };
};

bool should_cull_back_face(Gfx::FloatMatrix4x4 const& accumulated_matrix, Gfx::FloatMatrix4x4 const& plane_root_matrix);

struct ClipPathData {
    Gfx::Path path;
    DevicePixelRect bounding_rect;
    Gfx::WindingRule fill_rule;
};

struct EffectsData {
    float opacity { 1.0f };
    Gfx::CompositingAndBlendingOperator blend_mode { Gfx::CompositingAndBlendingOperator::Normal };
    Optional<Gfx::Filter> gfx_filter;
};

enum class MaskLayerOrigin : u8 {
    CssMaskLayers,
    SvgMask,
    SvgClip,
};

struct MaskData {
    DevicePixelRect rect;
    Gfx::MaskKind kind { Gfx::MaskKind::Alpha };
    MaskLayerOrigin origin { MaskLayerOrigin::CssMaskLayers };
};

// One scroll node's contribution to the default scroll shift of an anchor-positioned box, masked to the axes in which
// the box compensates for scroll. Nodes that move the box but not its default anchor contribute negated.
struct AnchorScrollShift {
    SpatialNodeIndex scroll_node_index;
    bool negate { false };
    bool compensate_horizontal_scroll { true };
    bool compensate_vertical_scroll { true };

    Gfx::FloatPoint masked_offset(ScrollStateSnapshot const&) const;
};

using SpatialData = Variant<ScrollData, StickyData, TransformData, PerspectiveData, BackfaceVisibilityData, AnchorScrollShift>;
using FrameData = Variant<ClipData, ClipPathData, EffectsData, MaskData>;

struct SpatialNode {
    SpatialData data;
    SpatialNodeIndex parent {};
};

// A frame's geometry is expressed in the space of the spatial node it records. That node and
// the spatial node of every context applying the frame lie on one root path: the frame's node
// is an ancestor-or-self of the context's, except for a fixed-background context, which is
// rooted above the scroll nodes its frames record in and so has them hanging below its node.
struct FrameNode {
    FrameData data;
    FrameNodeIndex parent { NO_FRAME_NODE };
    SpatialNodeIndex spatial {};
    bool clips_everything { false };
};

class AccumulatedVisualContextTree {
public:
    enum class IncludeVisualViewportTransform {
        No,
        Yes,
    };

    enum class ClipBehavior {
        Respect,
        // Transform the point without rejecting it against clip rects and clip paths. Used when searching for the
        // closest caret position within a scope the point may lie entirely outside of.
        Ignore,
    };

    static WEB_API AccumulatedVisualContextTree create();
    static WEB_API AccumulatedVisualContextTree create(TransformData visual_viewport_transform);
    // For nested display list trees (masks, patterns, background tiles): the given transform
    // becomes the root node. Unlike a document tree's visual viewport root, this root is real
    // content placement, so queries asked to exclude the visual viewport transform include it.
    static WEB_API AccumulatedVisualContextTree create_with_content_root(TransformData content_transform);
    static WEB_API AccumulatedVisualContextTree create_with_content_offset(Gfx::IntPoint content_offset);

    AccumulatedVisualContextTree(AccumulatedVisualContextTree const&) = default;
    AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree const&) = default;
    AccumulatedVisualContextTree(AccumulatedVisualContextTree&&) = default;
    AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree&&) = default;
    ~AccumulatedVisualContextTree() = default;

    u64 version() const { return m_version; }

    WEB_API SpatialNodeIndex append_spatial(SpatialData, SpatialNodeIndex parent);
    WEB_API FrameNodeIndex append_frame(FrameData, FrameNodeIndex parent, SpatialNodeIndex spatial);
    WEB_API void set_visual_viewport_transform(TransformData);
    WEB_API void reuse_version_from(AccumulatedVisualContextTree const&);

    SpatialNode const& spatial_node_at(SpatialNodeIndex index) const { return m_spatial_nodes[index.value()]; }
    SpatialNode& spatial_node_at(SpatialNodeIndex index) { return m_spatial_nodes[index.value()]; }
    FrameNode const& frame_node_at(FrameNodeIndex index) const { return m_frame_nodes[index.value()]; }
    WEB_API void set_frame_data(FrameNodeIndex, FrameData);
    ReadonlySpan<SpatialNode> spatial_nodes() const { return m_spatial_nodes.span(); }
    ReadonlySpan<FrameNode> frame_nodes() const { return m_frame_nodes.span(); }
    bool root_is_visual_viewport() const { return m_root_is_visual_viewport; }
    Optional<FrameNodeIndex> root_isolation_frame() const { return m_root_isolation_frame; }
    void set_root_isolation_frame(FrameNodeIndex frame)
    {
        VERIFY(frame.value() < m_frame_nodes.size());
        m_root_isolation_frame = frame;
    }

    WEB_API Optional<Gfx::FloatPoint> transform_point_for_hit_test(ContextRef, Gfx::FloatPoint, ScrollStateSnapshot const&, ClipBehavior = ClipBehavior::Respect) const;
    Gfx::FloatPoint inverse_transform_point(SpatialNodeIndex, Gfx::FloatPoint) const;
    Gfx::FloatRect transform_rect_to_viewport(SpatialNodeIndex, Gfx::FloatRect const&, ScrollStateSnapshot const&, IncludeVisualViewportTransform = IncludeVisualViewportTransform::Yes) const;
    // Sum of the snapshot entries along the scroll-parent chain from the given scroll-like node.
    Gfx::FloatPoint cumulative_scroll_chain_offset(SpatialNodeIndex, ScrollStateSnapshot const&) const;
    Gfx::FloatMatrix4x4 accumulated_matrix(SpatialNodeIndex, ScrollStateSnapshot const&, IncludeVisualViewportTransform) const;
    Gfx::FloatSize accumulated_2d_scale(SpatialNodeIndex, ScrollStateSnapshot const&, IncludeVisualViewportTransform) const;
    void dump_spatial_node(SpatialNodeIndex, StringBuilder&) const;
    void dump_frame_node(FrameNodeIndex, StringBuilder&) const;

    WEB_API Vector<bool> frames_with_empty_effective_clip() const;

private:
    AccumulatedVisualContextTree(u64 version, TransformData root_transform, bool root_is_visual_viewport);
    AccumulatedVisualContextTree(u64 version, Vector<SpatialNode>&& spatial_nodes, Vector<FrameNode>&& frame_nodes, bool root_is_visual_viewport);

    Vector<SpatialNodeIndex, 8> build_ancestor_chain(SpatialNodeIndex index) const;
    bool chain_contains_3d_transform(SpatialNodeIndex index) const;

    u64 m_version { 0 };
    Vector<SpatialNode> m_spatial_nodes;
    Vector<FrameNode> m_frame_nodes;
    bool m_root_is_visual_viewport { true };
    Optional<FrameNodeIndex> m_root_isolation_frame;

    template<typename T>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, T const&);
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);
};

// Fills the snapshot entries of the tree's sticky nodes from the scroll containers' entries.
WEB_API void resolve_sticky_offsets(AccumulatedVisualContextTree const&, ScrollStateSnapshot&);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ScrollData const&);
template<>
WEB_API ErrorOr<Web::Painting::ScrollData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::StickyData const&);
template<>
WEB_API ErrorOr<Web::Painting::StickyData> decode(Decoder&);

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
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::BackfaceVisibilityData const&);
template<>
WEB_API ErrorOr<Web::Painting::BackfaceVisibilityData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ClipPathData const&);
template<>
WEB_API ErrorOr<Web::Painting::ClipPathData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::EffectsData const&);
template<>
WEB_API ErrorOr<Web::Painting::EffectsData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::MaskData const&);
template<>
WEB_API ErrorOr<Web::Painting::MaskData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::AnchorScrollShift const&);
template<>
WEB_API ErrorOr<Web::Painting::AnchorScrollShift> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::SpatialNode const&);
template<>
WEB_API ErrorOr<Web::Painting::SpatialNode> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::FrameNode const&);
template<>
WEB_API ErrorOr<Web::Painting::FrameNode> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ContextRef const&);
template<>
WEB_API ErrorOr<Web::Painting::ContextRef> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::AccumulatedVisualContextTree const&);
template<>
WEB_API ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder&);

}
