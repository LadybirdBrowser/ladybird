/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class ScrollStateSnapshot;

struct TransformWithOrigin {
    Gfx::FloatMatrix4x4 matrix;
    Gfx::FloatPoint origin;
};

// What a tree reports about the animations it carries, for test introspection.
struct VisualAnimationSummary {
    size_t count { 0 };
    double local_time_at_anchor_ms_of_first { 0 };
    bool share_timing_anchor { true };
    bool targets_are_valid { true };
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

    static WEB_API AccumulatedVisualContextTree adopt_rust_handle(void const* retained_tree);
    static WEB_API ErrorOr<AccumulatedVisualContextTree> from_serialized_bytes(ReadonlyBytes);

    WEB_API AccumulatedVisualContextTree(AccumulatedVisualContextTree const&);
    WEB_API AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree const&);
    WEB_API AccumulatedVisualContextTree(AccumulatedVisualContextTree&&);
    WEB_API AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree&&);
    WEB_API ~AccumulatedVisualContextTree();

    WEB_API u64 structural_epoch() const;
    WEB_API ByteBuffer serialize_to_bytes() const;
    void const* rust_handle() const { return m_rust_tree; }

    WEB_API bool has_visual_animations() const;
    WEB_API bool has_active_visual_animation_at(i64 monotonic_time_ns) const;
    WEB_API VisualAnimationSummary visual_animation_summary() const;
    // One flag per spatial node: whether a transform animation moves the node or an ancestor.
    WEB_API Vector<bool> spatial_nodes_in_subtrees_of_transform_animations() const;

    // Node counts cover all slots, live or dead, or only live slots.
    WEB_API size_t spatial_node_count() const;
    WEB_API bool context_is_valid(ContextRef) const;
    WEB_API size_t node_count() const;
    WEB_API size_t live_node_count() const;
    WEB_API TransformWithOrigin visual_viewport_transform() const;
    WEB_API AccumulatedVisualContextTree with_visual_viewport_transform(TransformWithOrigin const&) const;
    // A copy whose nodes carry the values the animations take at the time.
    WEB_API AccumulatedVisualContextTree with_visual_animation_samples(i64 monotonic_time_ns) const;
    WEB_API Optional<float> effects_opacity(EffectNodeIndex) const;
    // The sampled background color of a recorded fill's animation effect.
    WEB_API Optional<Gfx::Color> sampled_background_color(EffectNodeIndex) const;
    WEB_API Vector<bool> spatial_nodes_in_subtrees_of(ReadonlySpan<SpatialNodeIndex> roots) const;

    WEB_API Optional<Gfx::FloatPoint> transform_point_for_hit_test(ContextRef, Gfx::FloatPoint, ScrollStateSnapshot const&, ClipBehavior = ClipBehavior::Respect) const;
    WEB_API Gfx::FloatPoint inverse_transform_point(SpatialNodeIndex, Gfx::FloatPoint) const;
    WEB_API Gfx::FloatRect transform_rect_to_viewport(SpatialNodeIndex, Gfx::FloatRect const&, ScrollStateSnapshot const&, IncludeVisualViewportTransform = IncludeVisualViewportTransform::Yes) const;
    // Sum of the snapshot entries along the scroll-parent chain from the given scroll-like node.
    WEB_API Gfx::FloatPoint cumulative_scroll_chain_offset(SpatialNodeIndex, ScrollStateSnapshot const&) const;
    WEB_API Gfx::FloatMatrix4x4 accumulated_matrix(SpatialNodeIndex, ScrollStateSnapshot const&, IncludeVisualViewportTransform) const;

    WEB_API bool effect_is_isolated_by_layer(EffectNodeIndex) const;
    WEB_API bool has_unisolated_destination_reading_effect() const;
    WEB_API void for_each_effects_filter_bytes(Function<void(ReadonlyBytes)> const&) const;

private:
    explicit AccumulatedVisualContextTree(void const* retained_tree);

    void release_rust_handle();

    void const* m_rust_tree { nullptr };
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
