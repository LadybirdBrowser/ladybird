/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
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

class Paintable;
class ScrollStateSnapshot;

// The node's own VisualContextIndex keys the scroll offset snapshot; the paintable that owns the
// node and its sticky constraints live in the ScrollState entry addressed by state_slot, stamped
// at registration. The slot is process-local bookkeeping: it stays off the wire and takes no part
// in tree compatibility or damage comparisons.
struct ScrollData {
    bool is_sticky { false };
    ScrollStateSlot state_slot { NO_SCROLL_STATE_SLOT };
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
    Optional<VisualContextIndex> sorting_context_root_index {};
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
    VisualContextIndex plane_root_index;
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

// Translates by another scroll node's negated offset during display list replay, keeping fixed
// backgrounds stationary relative to the viewport regardless of scroll position.
struct ScrollCompensation {
    VisualContextIndex scroll_node_index;
};

// One scroll node's contribution to the default scroll shift of an anchor-positioned box, masked to the axes in which
// the box compensates for scroll. Nodes that move the box but not its default anchor contribute negated.
struct AnchorScrollShift {
    VisualContextIndex scroll_node_index;
    bool negate { false };
    bool compensate_horizontal_scroll { true };
    bool compensate_vertical_scroll { true };

    Gfx::FloatPoint masked_offset(ScrollStateSnapshot const&) const;
};

using VisualContextData = Variant<ScrollData, ClipData, TransformData, PerspectiveData, BackfaceVisibilityData, ClipPathData, EffectsData, ScrollCompensation, AnchorScrollShift, MaskData>;

struct AccumulatedVisualContextNode {
    VisualContextData data;
    VisualContextIndex parent_index {};
    size_t depth { 0 };
    bool has_empty_effective_clip { false };
};

// Marks a visual context node whose content belongs to no 3D rendering context.
static constexpr VisualContextIndex NO_SORTING_CONTEXT { NumericLimits<size_t>::max() };

// The plane and 3D rendering context that an established context's own plane renders into.
struct SortingContextLink {
    VisualContextIndex parent_context;
    VisualContextIndex parent_leaf;
};

// Per-node 3D rendering context membership: the plane each node's content renders into and the context that
// sorts that plane. A tree without 3D rendering contexts resolves to empty per-node vectors.
struct SortingContexts {
    HashMap<size_t, SortingContextLink> links;
    Vector<VisualContextIndex> leaf_by_node;
    Vector<VisualContextIndex> context_by_node;

    bool is_empty() const { return leaf_by_node.is_empty(); }
    VisualContextIndex outermost_context_of(VisualContextIndex) const;
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

    WEB_API VisualContextIndex append(VisualContextData data, VisualContextIndex parent_index);
    WEB_API void set_visual_viewport_transform(TransformData);
    WEB_API bool is_compatible_with(AccumulatedVisualContextTree const&) const;
    WEB_API void reuse_version_from(AccumulatedVisualContextTree const&);

    AccumulatedVisualContextNode const& node_at(VisualContextIndex index) const { return m_nodes[index.value()]; }
    AccumulatedVisualContextNode& node_at(VisualContextIndex index) { return m_nodes[index.value()]; }
    ReadonlySpan<AccumulatedVisualContextNode> nodes() const { return m_nodes.span(); }
    bool root_is_visual_viewport() const { return m_root_is_visual_viewport; }

    SortingContexts resolve_sorting_contexts() const;
    Optional<float> plane_depth_at_point_for_hit_test(VisualContextIndex plane_node_index, Gfx::FloatPoint, ScrollStateSnapshot const&) const;
    Optional<Gfx::FloatPoint> transform_point_for_hit_test(VisualContextIndex, Gfx::FloatPoint, ScrollStateSnapshot const&, ClipBehavior = ClipBehavior::Respect) const;
    Gfx::FloatPoint inverse_transform_point(VisualContextIndex, Gfx::FloatPoint) const;
    Gfx::FloatRect transform_rect_to_viewport(VisualContextIndex, Gfx::FloatRect const&, ScrollStateSnapshot const&, IncludeVisualViewportTransform = IncludeVisualViewportTransform::Yes) const;
    Gfx::FloatMatrix4x4 accumulated_matrix(VisualContextIndex, ScrollStateSnapshot const&, IncludeVisualViewportTransform) const;
    Gfx::FloatSize accumulated_2d_scale(VisualContextIndex, ScrollStateSnapshot const&, IncludeVisualViewportTransform) const;
    void dump(VisualContextIndex, StringBuilder&) const;

    bool has_empty_effective_clip(VisualContextIndex i) const { return m_nodes[i.value()].has_empty_effective_clip; }

private:
    AccumulatedVisualContextTree(u64 version, Vector<AccumulatedVisualContextNode>&& nodes, bool root_is_visual_viewport)
        : m_version(version)
        , m_nodes(move(nodes))
        , m_root_is_visual_viewport(root_is_visual_viewport)
    {
    }

    Vector<size_t, 8> build_ancestor_chain(VisualContextIndex index) const;
    bool chain_contains_3d_transform(VisualContextIndex index) const;

    u64 m_version { 0 };
    Vector<AccumulatedVisualContextNode> m_nodes;
    bool m_root_is_visual_viewport { true };

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
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::ScrollCompensation const&);
template<>
WEB_API ErrorOr<Web::Painting::ScrollCompensation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::AnchorScrollShift const&);
template<>
WEB_API ErrorOr<Web::Painting::AnchorScrollShift> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::AccumulatedVisualContextNode const&);
template<>
WEB_API ErrorOr<Web::Painting::AccumulatedVisualContextNode> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Painting::AccumulatedVisualContextTree const&);
template<>
WEB_API ErrorOr<Web::Painting::AccumulatedVisualContextTree> decode(Decoder&);

}
