/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ContextScene.h>
#include <LibCompositing/DisplayList/DisplayListDamage.h>

namespace Compositor {

static void update_visual_animation_sampling_state(Compositing::AccumulatedVisualContextTree const& visual_context_tree, Optional<i64>& sample_time_ns, bool& has_active_animations)
{
    auto now = MonotonicTime::now();
    has_active_animations = visual_context_tree.has_active_visual_animation_at(now.nanoseconds());
    // A replacement tree can be presented before the next vsync. Keep the most recent sample so that presentation
    // remains continuous, but sample dormant and completed descriptors at the current boundary when necessary.
    if (!visual_context_tree.has_visual_animations())
        sample_time_ns.clear();
    else if (!has_active_animations)
        sample_time_ns = now.nanoseconds();
}

void ContextScene::install(NonnullRefPtr<Compositing::DisplayList> display_list, Compositing::AccumulatedVisualContextTree visual_context_tree)
{
    invalidate_visual_context_tree_for_compositing();
    m_display_list = move(display_list);
    m_visual_context_tree = move(visual_context_tree);
    update_visual_animation_sampling_state(*m_visual_context_tree, m_visual_animation_sample_time_ns, m_has_active_visual_animations);
}

bool ContextScene::replace_visual_context_tree(Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    if (!m_display_list || m_display_list->compatible_visual_context_tree_structural_epoch() != visual_context_tree.structural_epoch()) {
        dbgln("Compositor: Dropping stale visual context tree update (tree epoch {}, display list epoch {})",
            visual_context_tree.structural_epoch(),
            m_display_list ? m_display_list->compatible_visual_context_tree_structural_epoch() : 0);
        return false;
    }
    invalidate_visual_context_tree_for_compositing();
    apply_resource_transaction(move(resource_transaction));
    m_visual_context_tree = move(visual_context_tree);
    update_visual_animation_sampling_state(*m_visual_context_tree, m_visual_animation_sample_time_ns, m_has_active_visual_animations);
    return true;
}

void ContextScene::apply_resource_transaction(Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    m_raster_cache.evict(m_resource_storage.apply_transaction(move(resource_transaction)));
}

void ContextScene::set_video_sink(Compositing::VideoSinkResourceId frame_id, RefPtr<Media::DisplayingVideoSink> sink)
{
    m_resource_storage.set_video_sink(frame_id, move(sink));
}

Compositing::AccumulatedVisualContextTree const& ContextScene::current_visual_context_tree() const
{
    VERIFY(m_display_list);
    VERIFY(m_visual_context_tree.has_value());
    return m_visual_context_tree.value();
}

Compositing::AccumulatedVisualContextTree const& ContextScene::visual_context_tree_for_compositing()
{
    if (m_async_visual_viewport_transform.has_value() && !m_visual_context_tree_for_compositing.has_value()) {
        m_sampled_visual_context_tree.clear();
        m_visual_context_tree_for_compositing = current_visual_context_tree().with_visual_viewport_transform(*m_async_visual_viewport_transform);
    }

    auto const& unsampled_tree = m_visual_context_tree_for_compositing.has_value()
        ? *m_visual_context_tree_for_compositing
        : *m_visual_context_tree;
    if (!m_visual_animation_sample_time_ns.has_value())
        return unsampled_tree;
    if (!m_sampled_visual_context_tree.has_value())
        m_sampled_visual_context_tree = unsampled_tree.with_visual_animation_samples(*m_visual_animation_sample_time_ns);
    return *m_sampled_visual_context_tree;
}

Compositing::TransformWithOrigin ContextScene::visual_viewport_transform() const
{
    return m_async_visual_viewport_transform.value_or(m_visual_context_tree->visual_viewport_transform());
}

Optional<float> ContextScene::visual_viewport_scale() const
{
    if (!m_visual_context_tree.has_value())
        return {};

    if (m_async_visual_viewport_transform.has_value())
        return m_async_visual_viewport_transform->matrix[0, 0];

    return m_visual_context_tree->visual_viewport_transform().matrix[0, 0];
}

float ContextScene::visual_viewport_magnification() const
{
    return max(visual_viewport_scale().value_or(1.0f), 1.0f);
}

void ContextScene::set_async_visual_viewport_transform(Compositing::TransformWithOrigin transform)
{
    invalidate_visual_context_tree_for_compositing();
    m_async_visual_viewport_transform = transform;
}

void ContextScene::drop_async_visual_viewport_transform()
{
    invalidate_visual_context_tree_for_compositing();
    m_async_visual_viewport_transform.clear();
}

bool ContextScene::advance_visual_animations(MonotonicTime now)
{
    if (!m_visual_context_tree->has_visual_animations())
        return false;
    m_sampled_visual_context_tree.clear();
    m_visual_animation_sample_time_ns = now.nanoseconds();
    visual_context_tree_for_compositing();

    m_has_active_visual_animations = m_visual_context_tree->has_active_visual_animation_at(now.nanoseconds());
    return m_has_active_visual_animations;
}

bool ContextScene::visual_animations_need_frame(Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::IntSize viewport_size)
{
    if (!m_has_active_visual_animations)
        return false;
    if (m_animated_content_may_affect_viewport.has_value())
        return *m_animated_content_may_affect_viewport;

    auto sample_time_ns = m_visual_animation_sample_time_ns.value_or(MonotonicTime::now().nanoseconds());
    auto tree = m_async_visual_viewport_transform.has_value()
        ? current_visual_context_tree().with_visual_viewport_transform(*m_async_visual_viewport_transform)
        : current_visual_context_tree();
    auto effect = Compositing::animated_content_may_affect_viewport(
        *m_display_list, tree, scroll_state_snapshot, { {}, viewport_size }, sample_time_ns);
    if (effect.stable_until_scene_changes)
        m_animated_content_may_affect_viewport = effect.may_affect_viewport;
    return effect.may_affect_viewport;
}

void ContextScene::invalidate_visual_context_tree_for_compositing()
{
    m_animated_content_may_affect_viewport.clear();
    m_sampled_visual_context_tree.clear();
    m_visual_context_tree_for_compositing.clear();
}

}
