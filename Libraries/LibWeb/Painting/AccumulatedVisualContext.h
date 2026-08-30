/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/DistinctNumeric.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/WindingRule.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Compositor/VisualAnimation.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class ScrollStateSnapshot;

enum class ClipMode : u8;

struct TransformWithOrigin {
    Gfx::FloatMatrix4x4 matrix;
    Gfx::FloatPoint origin;
};

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

struct ClipData {
    Gfx::FloatRect rect;
    Gfx::CornerRadii corner_radii;
    ClipMode mode;
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
};

struct PerspectiveData {
    Gfx::FloatMatrix4x4 matrix;
    bool flattens_inherited_transform { false };
};

struct BackfaceVisibilityData {
    SpatialNodeIndex plane_root_index;
    bool flattens_inherited_transform { false };
};

struct ClipPathData {
    Gfx::Path path;
    DevicePixelRect bounding_rect;
    Gfx::WindingRule fill_rule;
};

struct EffectsData {
    float opacity { 1.0f };
    Gfx::CompositingAndBlendingOperator blend_mode { Gfx::CompositingAndBlendingOperator::Normal };
    Optional<ByteBuffer> filter_bytes;
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
    struct VisualAnimationOriginalValues {
        struct Opacity {
            FrameNodeIndex node_index;
            float value { 1 };
        };

        struct Transform {
            SpatialNodeIndex node_index;
            Gfx::FloatMatrix4x4 value;
        };

        bool is_empty() const { return opacities.is_empty() && transforms.is_empty(); }
        void clear()
        {
            opacities.clear();
            transforms.clear();
        }

        Vector<Opacity> opacities;
        Vector<Transform> transforms;
    };

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

    static WEB_API AccumulatedVisualContextTree create(TransformData visual_viewport_transform);
    // For nested display list trees (masks, patterns, background tiles): the given transform
    // becomes the root node. Unlike a document tree's visual viewport root, this root is real
    // content placement, so queries asked to exclude the visual viewport transform include it.
    static WEB_API AccumulatedVisualContextTree create_with_content_root(TransformData content_transform);
    static WEB_API AccumulatedVisualContextTree materialize_from_rust(void const* retained_tree);
    static WEB_API ErrorOr<AccumulatedVisualContextTree> from_serialized_bytes(ReadonlyBytes);

    WEB_API AccumulatedVisualContextTree(AccumulatedVisualContextTree const&);
    WEB_API AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree const&);
    WEB_API AccumulatedVisualContextTree(AccumulatedVisualContextTree&&);
    WEB_API AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree&&);
    WEB_API ~AccumulatedVisualContextTree();

    WEB_API u64 version() const;
    WEB_API ByteBuffer serialize_to_bytes() const;

    // The Rust tree this mirror was materialized from, retained by this value.
    void const* rust_tree() const { return m_rust_tree; }
    WEB_API void adopt_rust_tree(void const* retained_tree);
    WEB_API void release_rust_tree();

    WEB_API SpatialNodeIndex append_spatial(SpatialData, SpatialNodeIndex parent);
    WEB_API FrameNodeIndex append_frame(FrameData, FrameNodeIndex parent, SpatialNodeIndex spatial);

    SpatialNode const& spatial_node_at(SpatialNodeIndex index) const { return m_spatial_nodes[index.value()]; }
    SpatialNode& spatial_node_at(SpatialNodeIndex index) { return m_spatial_nodes[index.value()]; }
    FrameNode const& frame_node_at(FrameNodeIndex index) const { return m_frame_nodes[index.value()]; }
    FrameNode& frame_node_at(FrameNodeIndex index) { return m_frame_nodes[index.value()]; }
    WEB_API void set_frame_data(FrameNodeIndex, FrameData);
    ReadonlySpan<SpatialNode> spatial_nodes() const { return m_spatial_nodes.span(); }
    ReadonlySpan<FrameNode> frame_nodes() const { return m_frame_nodes.span(); }
    bool root_is_visual_viewport() const { return m_root_is_visual_viewport; }
    Optional<FrameNodeIndex> root_isolation_frame() const { return m_root_isolation_frame; }
    ReadonlySpan<Compositor::VisualAnimation> visual_animations() const { return m_visual_animations; }
    void set_visual_animations(Vector<Compositor::VisualAnimation> animations) { m_visual_animations = move(animations); }
    WEB_API void sample_visual_animations(i64 monotonic_time_ns, VisualAnimationOriginalValues&);
    WEB_API void restore_visual_animation_original_values(VisualAnimationOriginalValues&);
    void set_root_isolation_frame(FrameNodeIndex frame)
    {
        VERIFY(frame.value() < m_frame_nodes.size());
        m_root_isolation_frame = frame;
    }

    WEB_API size_t spatial_node_count() const;
    WEB_API size_t frame_node_count() const;
    WEB_API TransformWithOrigin visual_viewport_transform() const;
    WEB_API AccumulatedVisualContextTree with_visual_viewport_transform(TransformWithOrigin const&) const;
    WEB_API AccumulatedVisualContextTree with_visual_animation_samples(i64 monotonic_time_ns) const;
    WEB_API bool visual_animation_targets_are_valid(Compositor::VisualAnimation const&) const;
    WEB_API Optional<float> effects_opacity(FrameNodeIndex) const;
    WEB_API Vector<bool> spatial_nodes_in_subtrees_of(ReadonlySpan<SpatialNodeIndex> roots) const;

    WEB_API Optional<Gfx::FloatPoint> transform_point_for_hit_test(ContextRef, Gfx::FloatPoint, ScrollStateSnapshot const&, ClipBehavior = ClipBehavior::Respect) const;
    WEB_API Gfx::FloatPoint inverse_transform_point(SpatialNodeIndex, Gfx::FloatPoint) const;
    WEB_API Gfx::FloatRect transform_rect_to_viewport(SpatialNodeIndex, Gfx::FloatRect const&, ScrollStateSnapshot const&, IncludeVisualViewportTransform = IncludeVisualViewportTransform::Yes) const;
    // Sum of the snapshot entries along the scroll-parent chain from the given scroll-like node.
    WEB_API Gfx::FloatPoint cumulative_scroll_chain_offset(SpatialNodeIndex, ScrollStateSnapshot const&) const;
    WEB_API Gfx::FloatMatrix4x4 accumulated_matrix(SpatialNodeIndex, ScrollStateSnapshot const&, IncludeVisualViewportTransform) const;

    WEB_API bool frame_is_isolated_by_layer_frame(FrameNodeIndex) const;
    WEB_API bool has_unisolated_blending_frame() const;
    WEB_API void for_each_effects_filter_bytes(Function<void(ReadonlyBytes)> const&) const;
    WEB_API void dump(StringBuilder&, ReadonlySpan<DisplayListCommandRun>, Function<Optional<String>(SpatialNodeIndex)> const& spatial_node_owner_label, Function<Optional<String>(FrameNodeIndex)> const& frame_node_owner_label) const;

    WEB_API Vector<bool> frames_with_empty_effective_clip() const;

private:
    AccumulatedVisualContextTree(TransformData root_transform, bool root_is_visual_viewport);

    void const* m_rust_tree { nullptr };
    Vector<SpatialNode> m_spatial_nodes;
    Vector<FrameNode> m_frame_nodes;
    bool m_root_is_visual_viewport { true };
    Optional<FrameNodeIndex> m_root_isolation_frame;
    Vector<Compositor::VisualAnimation> m_visual_animations;

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
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::AccumulatedVisualContextTree const&);
template<>
WEB_API ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder&);

}
