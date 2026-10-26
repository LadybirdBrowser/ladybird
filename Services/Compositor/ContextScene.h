/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <Compositor/DisplayListRasterCache.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/Size.h>
#include <LibMedia/Forward.h>

namespace Compositor {

// What WebContent last sent a context to draw, and the visual context tree it is drawn with: the one WebContent sent,
// with the visual viewport this context has pinch-zoomed or panned since, and with its visual animations sampled.
class ContextScene {
    AK_MAKE_NONCOPYABLE(ContextScene);
    AK_MAKE_NONMOVABLE(ContextScene);

public:
    ContextScene() = default;

    void install(NonnullRefPtr<Compositing::DisplayList>, Compositing::AccumulatedVisualContextTree);
    // Returns false, leaving everything as it was, for a tree that does not fit the installed display list.
    bool replace_visual_context_tree(Compositing::AccumulatedVisualContextTree, Compositing::DisplayListResourceTransaction&&);
    void apply_resource_transaction(Compositing::DisplayListResourceTransaction&&);
    void set_video_sink(Compositing::VideoSinkResourceId, RefPtr<Media::DisplayingVideoSink>);
    HashMap<u64, Media::VideoSinkHandle> const& video_sink_handles() const { return m_resource_storage.video_sink_handles(); }

    RefPtr<Compositing::DisplayList const> const& display_list() const { return m_display_list; }
    Optional<Compositing::AccumulatedVisualContextTree> const& visual_context_tree() const { return m_visual_context_tree; }
    Compositing::AccumulatedVisualContextTree const& current_visual_context_tree() const;
    Compositing::AccumulatedVisualContextTree const& visual_context_tree_for_compositing();
    Compositing::DisplayListResourceStorage& resource_storage() { return m_resource_storage; }
    DisplayListRasterCache& raster_cache() { return m_raster_cache; }

    Compositing::TransformWithOrigin visual_viewport_transform() const;
    Optional<float> visual_viewport_scale() const;
    // How many screen pixels a zoomed visual viewport shows one pixel of the layout viewport with. Scrollers scroll and
    // stretch in the pixels of the layout viewport.
    float visual_viewport_magnification() const;
    bool has_async_visual_viewport_transform() const { return m_async_visual_viewport_transform.has_value(); }
    void set_async_visual_viewport_transform(Compositing::TransformWithOrigin);
    void drop_async_visual_viewport_transform();

    bool advance_visual_animations(MonotonicTime now);
    bool visual_animations_need_frame(Compositing::ScrollStateSnapshot const&, Gfx::IntSize viewport_size);
    void forget_whether_animated_content_may_affect_viewport() { m_animated_content_may_affect_viewport.clear(); }

private:
    void invalidate_visual_context_tree_for_compositing();

    RefPtr<Compositing::DisplayList const> m_display_list;
    Optional<Compositing::AccumulatedVisualContextTree> m_visual_context_tree;
    Optional<Compositing::TransformWithOrigin> m_async_visual_viewport_transform;
    Optional<Compositing::AccumulatedVisualContextTree> m_visual_context_tree_for_compositing;
    Optional<Compositing::AccumulatedVisualContextTree> m_sampled_visual_context_tree;
    Optional<i64> m_visual_animation_sample_time_ns;
    bool m_has_active_visual_animations { false };
    Optional<bool> m_animated_content_may_affect_viewport;
    Compositing::DisplayListResourceStorage m_resource_storage;
    DisplayListRasterCache m_raster_cache;
};

}
