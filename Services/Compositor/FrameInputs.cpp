/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <Compositor/FrameInputs.h>

namespace Compositor {

FrameInputs::FrameInputs(RustFFI::FrameInputs* frame_inputs, Compositing::AccumulatedVisualContextTree visual_context_tree)
    : m_frame_inputs(frame_inputs)
    , m_visual_context_tree(move(visual_context_tree))
{
}

FrameInputs FrameInputs::adopt(RustFFI::FrameInputs* frame_inputs)
{
    VERIFY(frame_inputs);
    return { frame_inputs, Compositing::AccumulatedVisualContextTree::adopt_rust_handle(RustFFI::compositor_frame_inputs_retain_visual_context_tree(frame_inputs)) };
}

FrameInputs::FrameInputs(FrameInputs&& other)
    : m_frame_inputs(exchange(other.m_frame_inputs, nullptr))
    , m_visual_context_tree(move(other.m_visual_context_tree))
{
}

FrameInputs& FrameInputs::operator=(FrameInputs&& other)
{
    if (this == &other)
        return *this;
    if (m_frame_inputs)
        RustFFI::compositor_frame_inputs_destroy(m_frame_inputs);
    m_frame_inputs = exchange(other.m_frame_inputs, nullptr);
    m_visual_context_tree = move(other.m_visual_context_tree);
    return *this;
}

FrameInputs::~FrameInputs()
{
    if (m_frame_inputs)
        RustFFI::compositor_frame_inputs_destroy(m_frame_inputs);
}

ReadonlySpan<ScrollbarOfFrame> FrameInputs::scrollbars() const
{
    size_t count = 0;
    auto const* scrollbars = RustFFI::compositor_frame_inputs_scrollbars(m_frame_inputs, &count);
    return { scrollbars, count };
}

void FrameInputs::replay(Compositing::DisplayList const& display_list, Compositing::RustFFI::FfiRasterPainter const& painter, ReplaySelection const& selection) const
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == m_visual_context_tree.structural_epoch());
    RustFFI::compositor_frame_inputs_replay(m_frame_inputs, display_list.rust_handle(), display_list.root_group_color(), &selection, &painter);
}

Vector<PresentedScrollbar> FrameInputs::scrollbars_painted_by_compositor() const
{
    Vector<PresentedScrollbar> presented_scrollbars;
    for (auto const& scrollbar : scrollbars()) {
        if (!scrollbar.is_painted_by_compositor)
            continue;
        // The gutter of a thin scrollbar is not painted, so the gutter grows out of the thin track as it fades in.
        auto track_alpha = roundf(static_cast<float>(scrollbar.track_color.alpha()) * scrollbar.expansion);
        presented_scrollbars.append({
            .gutter_rect = scrollbar.gutter_rect,
            .thumb_rect = scrollbar.thumb_rect,
            .thumb_color = scrollbar.thumb_color,
            .thumb_outline_color = scrollbar.thumb_outline_color,
            .track_color = scrollbar.track_color.with_alpha(static_cast<u8>(track_alpha)),
            .opacity = scrollbar.opacity,
        });
    }
    return presented_scrollbars;
}

RustFFI::ContextLayerPlan* FrameInputs::build_layer_plan(Compositing::DisplayList const& display_list, Gfx::IntRect viewport_rect, ReadonlySpan<RustFFI::FfiLiveMedia> active_media) const
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == m_visual_context_tree.structural_epoch());
    return RustFFI::compositor_frame_inputs_build_layer_plan(m_frame_inputs, display_list.rust_handle(), viewport_rect, active_media.data(), active_media.size());
}

bool FrameInputs::places_content_like(FrameInputs const& other) const
{
    return RustFFI::compositor_frame_inputs_place_content_alike(m_frame_inputs, other.m_frame_inputs);
}

DisplayListDamage FrameInputs::damage_since(Compositing::DisplayList const& display_list, FrameInputs const& last_frame_inputs, Compositing::DisplayList const& last_display_list, Gfx::IntRect viewport_rect, LiveContentChanges live_content_changes, RustFFI::ContextLayerPlan const* last_layer_plan, RustFFI::ContextLayerPlan const* layer_plan) const
{
    VERIFY(!last_layer_plan == !layer_plan);
    Gfx::IntRect damage_rect;
    Vector<Gfx::IntRect> layer_damage_rects;
    if (layer_plan)
        layer_damage_rects.resize(RustFFI::compositor_layer_plan_layer_count(layer_plan));
    u32 unbounded_layer_bits = 0;
    DisplayListDamage damage;
    bool damage_is_bounded = RustFFI::compositor_frame_inputs_damage_since(
        m_frame_inputs, display_list.rust_handle(),
        last_frame_inputs.m_frame_inputs, last_display_list.rust_handle(),
        viewport_rect,
        live_content_changes.canvases.data(), live_content_changes.canvases.size(),
        live_content_changes.video_sinks.data(), live_content_changes.video_sinks.size(),
        live_content_changes.image_frames.data(), live_content_changes.image_frames.size(),
        live_content_changes.forced_damage_rect,
        live_content_changes.offsets_of_records_composited_as_media_layers.data(), live_content_changes.offsets_of_records_composited_as_media_layers.size(),
        last_layer_plan,
        layer_plan,
        &damage_rect,
        layer_damage_rects.data(),
        &unbounded_layer_bits);
    if (damage_is_bounded)
        damage.rect = damage_rect;
    damage.layer_rects.ensure_capacity(layer_damage_rects.size());
    for (size_t index = 0; index < layer_damage_rects.size(); ++index) {
        if ((unbounded_layer_bits & (1u << index)) != 0)
            damage.layer_rects.unchecked_append({});
        else
            damage.layer_rects.unchecked_append(layer_damage_rects[index]);
    }
    return damage;
}

}
