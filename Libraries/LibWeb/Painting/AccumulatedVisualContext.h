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
#include <LibWeb/Compositor/VisualAnimation.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class ScrollStateSnapshot;

struct TransformWithOrigin {
    Gfx::FloatMatrix4x4 matrix;
    Gfx::FloatPoint origin;
};

class VisualAnimationList : public RefCounted<VisualAnimationList> {
public:
    explicit VisualAnimationList(Vector<Compositor::VisualAnimation> animations)
        : animations(move(animations))
    {
    }

    Vector<Compositor::VisualAnimation> animations;
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

    ReadonlySpan<Compositor::VisualAnimation> visual_animations() const { return m_visual_animations ? m_visual_animations->animations.span() : ReadonlySpan<Compositor::VisualAnimation> {}; }
    RefPtr<VisualAnimationList const> visual_animation_list() const { return m_visual_animations; }
    void set_visual_animations(RefPtr<VisualAnimationList const> animations) { m_visual_animations = move(animations); }
    WEB_API void set_visual_animations(Vector<Compositor::VisualAnimation>);

    WEB_API size_t spatial_node_count() const;
    WEB_API size_t frame_node_count() const;
    WEB_API size_t live_spatial_node_count() const;
    WEB_API size_t live_frame_node_count() const;
    WEB_API TransformWithOrigin visual_viewport_transform() const;
    WEB_API AccumulatedVisualContextTree with_visual_viewport_transform(TransformWithOrigin const&) const;
    WEB_API AccumulatedVisualContextTree with_visual_animation_samples(i64 monotonic_time_ns) const;
    WEB_API bool visual_animation_targets_are_valid(Compositor::VisualAnimation const&) const;
    WEB_API Optional<float> effects_opacity(FrameNodeIndex) const;
    WEB_API Optional<Gfx::Color> sampled_background_color(FrameNodeIndex) const;
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

private:
    explicit AccumulatedVisualContextTree(void const* retained_tree);

    void release_rust_handle();

    void const* m_rust_tree { nullptr };
    RefPtr<VisualAnimationList const> m_visual_animations;
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
