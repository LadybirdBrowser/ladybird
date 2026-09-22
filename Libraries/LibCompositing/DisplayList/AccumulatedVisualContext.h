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
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>

namespace Compositing {

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

class COMPOSITING_API AccumulatedVisualContextTree {
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

    static AccumulatedVisualContextTree adopt_rust_handle(void const* retained_tree);
    static ErrorOr<AccumulatedVisualContextTree> from_serialized_bytes(ReadonlyBytes);

    AccumulatedVisualContextTree(AccumulatedVisualContextTree const&);
    AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree const&);
    AccumulatedVisualContextTree(AccumulatedVisualContextTree&&);
    AccumulatedVisualContextTree& operator=(AccumulatedVisualContextTree&&);
    ~AccumulatedVisualContextTree();

    u64 structural_epoch() const;
    ByteBuffer serialize_to_bytes() const;
    void const* rust_handle() const { return m_rust_tree; }

    bool has_visual_animations() const;
    bool has_active_visual_animation_at(i64 monotonic_time_ns) const;
    VisualAnimationSummary visual_animation_summary() const;
    // One flag per spatial node: whether a transform animation moves the node or an ancestor.
    Vector<bool> spatial_nodes_in_subtrees_of_transform_animations() const;

    // Node counts cover all slots, live or dead, or only live slots.
    size_t spatial_node_count() const;
    bool context_is_valid(ContextRef) const;
    size_t node_count() const;
    size_t live_node_count() const;
    TransformWithOrigin visual_viewport_transform() const;
    AccumulatedVisualContextTree with_visual_viewport_transform(TransformWithOrigin const&) const;
    // A copy whose nodes carry the values the animations take at the time.
    AccumulatedVisualContextTree with_visual_animation_samples(i64 monotonic_time_ns) const;
    Optional<float> effects_opacity(EffectNodeIndex) const;
    // The sampled background color of a recorded fill's animation effect.
    Optional<Gfx::Color> sampled_background_color(EffectNodeIndex) const;
    Vector<bool> spatial_nodes_in_subtrees_of(ReadonlySpan<SpatialNodeIndex> roots) const;

    Optional<Gfx::FloatPoint> transform_point_for_hit_test(ContextRef, Gfx::FloatPoint, ScrollStateSnapshot const&, ClipBehavior = ClipBehavior::Respect) const;
    Gfx::FloatPoint inverse_transform_point(SpatialNodeIndex, Gfx::FloatPoint) const;
    Gfx::FloatRect transform_rect_to_viewport(SpatialNodeIndex, Gfx::FloatRect const&, ScrollStateSnapshot const&, IncludeVisualViewportTransform = IncludeVisualViewportTransform::Yes) const;
    // Sum of the snapshot entries along the scroll-parent chain from the given scroll-like node.
    Gfx::FloatPoint cumulative_scroll_chain_offset(SpatialNodeIndex, ScrollStateSnapshot const&) const;
    Gfx::FloatMatrix4x4 accumulated_matrix(SpatialNodeIndex, ScrollStateSnapshot const&, IncludeVisualViewportTransform) const;

    bool effect_is_isolated_by_layer(EffectNodeIndex) const;
    bool has_unisolated_destination_reading_effect() const;
    void for_each_effects_filter_bytes(Function<void(ReadonlyBytes)> const&) const;

private:
    explicit AccumulatedVisualContextTree(void const* retained_tree);

    void release_rust_handle();

    void const* m_rust_tree { nullptr };
};

// Fills the snapshot entries of the tree's sticky nodes from the scroll containers' entries.
COMPOSITING_API void resolve_sticky_offsets(AccumulatedVisualContextTree const&, ScrollStateSnapshot&);

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::AccumulatedVisualContextTree const&);
template<>
COMPOSITING_API ErrorOr<Compositing::AccumulatedVisualContextTree> decode(Decoder&);

}
