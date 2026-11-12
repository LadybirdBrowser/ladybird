/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ContextLayers.h>

namespace Compositor {

bool same_media(ReadonlySpan<LiveMedia> a, ReadonlySpan<LiveMedia> b)
{
    if (a.size() != b.size())
        return false;
    for (size_t index = 0; index < a.size(); ++index) {
        if (a[index].kind != b[index].kind || a[index].id != b[index].id)
            return false;
    }
    return true;
}

OwnPtr<LayerPlan> LayerPlan::build(FrameInputs const& frame_inputs, Compositing::DisplayList const& display_list, Gfx::IntRect viewport_rect, ReadonlySpan<LiveMedia> active_media)
{
    auto* plan = frame_inputs.build_layer_plan(display_list, viewport_rect, active_media);
    if (!plan)
        return {};
    return adopt_own(*new LayerPlan(plan));
}

LayerPlan::LayerPlan(RustFFI::ContextLayerPlan* plan)
    : m_plan(plan)
{
    auto layer_count = RustFFI::compositor_layer_plan_layer_count(m_plan);
    m_layers.ensure_capacity(layer_count);
    for (size_t index = 0; index < layer_count; ++index) {
        RustFFI::FfiLayer layer {};
        RustFFI::compositor_layer_plan_layer(m_plan, index, &layer);
        Optional<WindowServerPlacement> window_server_placement;
        if (layer.can_be_shown_by_window_server) {
            auto const& placement = layer.window_server_placement;
            auto const* transform = placement.content_to_viewport;
            Optional<RemoteSublayer::RoundedClip> rounded_clip;
            if (placement.has_rounded_clip) {
                rounded_clip = RemoteSublayer::RoundedClip {
                    .rect = placement.rounded_clip_rect,
                    .radius = placement.rounded_clip_radius,
                    .rounds_top_left = placement.rounds_top_left,
                    .rounds_top_right = placement.rounds_top_right,
                    .rounds_bottom_right = placement.rounds_bottom_right,
                    .rounds_bottom_left = placement.rounds_bottom_left,
                };
            }
            window_server_placement = WindowServerPlacement {
                .content_rect = placement.content_rect,
                .content_to_viewport = Gfx::AffineTransform { transform[0], transform[1], transform[2], transform[3], transform[4], transform[5] },
                .clip_rect = placement.clip_rect,
                .rounded_clip = rounded_clip,
                .scaling_mode = placement.scaling_mode,
            };
        }
        m_layers.unchecked_append({
            .kind = layer.kind,
            .media = layer.media,
            .rect = layer.rect,
            .record_spans = { layer.record_spans, layer.record_span_count },
            .window_server_placement = move(window_server_placement),
        });
        if (layer.kind == LayerKind::Media)
            m_media_record_offsets.append(layer.record_spans[0].begin_offset);
    }
}

LayerPlan::~LayerPlan()
{
    RustFFI::compositor_layer_plan_destroy(m_plan);
}

Gfx::IntRect LayerPlan::rect_of_media_layers_showing(ReadonlySpan<Compositing::CanvasId> canvases, ReadonlySpan<Compositing::VideoSinkResourceId> video_sinks) const
{
    Gfx::IntRect rect;
    for (auto const& layer : m_layers) {
        if (layer.kind != LayerKind::Media)
            continue;
        auto shows_changed_media = layer.media.kind == RustFFI::FfiLiveMediaKind::Canvas
            ? canvases.contains_slow(Compositing::CanvasId { layer.media.id })
            : video_sinks.contains_slow(Compositing::VideoSinkResourceId { layer.media.id });
        if (shows_changed_media)
            rect.unite(layer.rect);
    }
    return rect;
}

bool LayerPlan::has_same_layers_as(LayerPlan const& other) const
{
    return RustFFI::compositor_layer_plans_have_same_layers(m_plan, other.m_plan);
}

void MediaActivity::note_updates(ReadonlySpan<Update> updates, MonotonicTime now)
{
    m_updates.remove_all_matching([&](auto const& media_updates) {
        return now - media_updates.last_update > time_media_stays_active_after_an_update;
    });
    for (auto const& update : updates)
        note_update(update, now);
}

void MediaActivity::note_update(Update const& update, MonotonicTime now)
{
    for (auto& updates : m_updates) {
        if (updates.media.kind != update.media.kind || updates.media.id != update.media.id)
            continue;
        if (updates.content_version == update.content_version)
            return;
        if (now - updates.last_update <= longest_time_between_updates_that_activate_media)
            updates.active = true;
        updates.content_version = update.content_version;
        updates.last_update = now;
        return;
    }
    m_updates.append({ .media = update.media, .content_version = update.content_version, .last_update = now });
}

Vector<LiveMedia> MediaActivity::active_media(MonotonicTime now) const
{
    Vector<LiveMedia> media;
    for (auto const& updates : m_updates) {
        if (updates.active && now - updates.last_update <= time_media_stays_active_after_an_update)
            media.append(updates.media);
    }
    return media;
}

}
