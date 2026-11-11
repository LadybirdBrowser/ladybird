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
#include <LibCompositing/DisplayList/ContextRef.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>

namespace Compositing {

// What a tree reports about the animations it carries, for test introspection.
struct VisualAnimationSummary {
    size_t count { 0 };
    double local_time_at_anchor_ms_of_first { 0 };
    bool share_timing_anchor { true };
    bool targets_are_valid { true };
};

class COMPOSITING_API AccumulatedVisualContextTree {
public:
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
    VisualAnimationSummary visual_animation_summary() const;

    // Node counts cover all slots, live or dead, or only live slots.
    size_t node_count() const;
    size_t live_node_count() const;
    // A copy whose nodes carry the values the animations take at the time.
    AccumulatedVisualContextTree with_visual_animation_samples(i64 monotonic_time_ns) const;
    // The sampled background color of a recorded fill's animation effect.
    Optional<Gfx::Color> sampled_background_color(EffectNodeIndex) const;
    // Whether the node's scroll translations and the origins drawn in its space round to whole device pixels: no
    // transform but identities lies above it.
    bool translations_snap_to_device_pixels(SpatialNodeIndex) const;

    void for_each_effects_filter_image_frame_id(Function<void(u64)> const&) const;

private:
    explicit AccumulatedVisualContextTree(void const* retained_tree);

    void release_rust_handle();

    void const* m_rust_tree { nullptr };
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::AccumulatedVisualContextTree const&);
template<>
COMPOSITING_API ErrorOr<Compositing::AccumulatedVisualContextTree> decode(Decoder&);

}
