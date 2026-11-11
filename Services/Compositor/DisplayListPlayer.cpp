/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <Compositor/DisplayListPlayer.h>
#include <Compositor/PaintingSurface.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/PathData.h>

namespace Compositor {

using namespace Compositing;

void DisplayListPlayer::execute(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    DisplayListResourceStorage const& resource_storage,
    FrameInputs const* frame_inputs,
    RefPtr<Gfx::PaintingSurface> surface,
    OwnedCanvasSurfaces const* canvas_surfaces)
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    m_surface = surface;
    m_active_display_list = &display_list;
    m_active_visual_context_tree = &visual_context_tree;
    m_resource_storage = &resource_storage;
    m_canvas_surfaces = canvas_surfaces;
    execute_impl(display_list, frame_inputs);
    m_canvas_surfaces = nullptr;
    m_resource_storage = nullptr;
    m_active_visual_context_tree = nullptr;
    m_active_display_list = nullptr;
    m_surface = nullptr;
}

void DisplayListPlayer::execute_display_list_into_surface(DisplayList const& display_list, AccumulatedVisualContextTree const& visual_context_tree, Gfx::PaintingSurface& target_surface)
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    TemporaryChange surface_change { m_surface, RefPtr<Gfx::PaintingSurface> { target_surface } };
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    execute_impl(display_list, nullptr);
}

void DisplayListPlayer::execute_nested_display_list(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree)
{
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    TemporaryChange display_list_change { m_active_display_list, &display_list };
    TemporaryChange visual_context_tree_change { m_active_visual_context_tree, &visual_context_tree };
    VERIFY(m_resource_storage);
    execute_impl(display_list, nullptr);
}

// Builds the callbacks the Rust replay drives a player through.
struct DisplayListPlayer::ReplayCallbacks {
    static Compositing::RustFFI::FfiDisplayListReplayCallbacks for_player(DisplayListPlayer& player)
    {
        return {
            .context = &player,
            .canvas_matrix = [](void* context) -> Gfx::FloatMatrix4x4 { return static_cast<DisplayListPlayer*>(context)->canvas_matrix(); },
            .set_matrix = [](void* context, Gfx::FloatMatrix4x4 const* matrix) { static_cast<DisplayListPlayer*>(context)->set_matrix(*matrix); },
            .would_be_fully_clipped_by_painter = [](void* context, Gfx::IntRect rect) -> bool {
                return static_cast<DisplayListPlayer*>(context)->would_be_fully_clipped_by_painter(rect);
            },
            .push_clip = [](void* context, ReplayClip const* clip) { static_cast<DisplayListPlayer*>(context)->push_clip(*clip); },
            .push_clip_path = [](void* context, Gfx::FFI::PathGeometryData const* path, u64 path_identity, Gfx::WindingRule winding_rule) { static_cast<DisplayListPlayer*>(context)->push_clip_path(Gfx::PathData::from_ffi(*path), path_identity, winding_rule); },
            .push_layer = [](void* context, PlayedLayer const* layer) { static_cast<DisplayListPlayer*>(context)->push_layer(*layer); },
            .push_mask = [](void* context, ReplayMask const* mask, PlayedLayer const* backdrop) { static_cast<DisplayListPlayer*>(context)->push_mask(*mask, backdrop); },
            .begin_mask_content = [](void* context, ReplayMask const* mask) { static_cast<DisplayListPlayer*>(context)->begin_mask_content(*mask); },
            .end_mask = [](void* context) { static_cast<DisplayListPlayer*>(context)->end_mask(); },
            .begin_isolated_group = [](void* context, DrawIsolatedGroup const* group, FilterGraph const* filter) { static_cast<DisplayListPlayer*>(context)->begin_isolated_group(*group, *filter); },
            .begin_isolated_group_mask = [](void* context, Gfx::MaskKind kind) { static_cast<DisplayListPlayer*>(context)->begin_isolated_group_mask(kind); },
            .end_isolated_group = [](void* context) { static_cast<DisplayListPlayer*>(context)->end_isolated_group(); },
            .use_cached_tile_raster = [](void* context, u64 key, Gfx::IntSize size) -> bool { return static_cast<DisplayListPlayer*>(context)->use_cached_tile_raster(key, size); },
            .begin_tile_raster = [](void* context, Gfx::IntSize size) -> bool { return static_cast<DisplayListPlayer*>(context)->begin_tile_raster(size); },
            .end_tile_raster = [](void* context, u64 const* cache_key) { static_cast<DisplayListPlayer*>(context)->end_tile_raster(cache_key ? Optional<u64> { *cache_key } : Optional<u64> {}); },
            .pop = [](void* context) { static_cast<DisplayListPlayer*>(context)->pop(); },
            .push_device_space_plane_clip = [](void* context, Gfx::FloatVector3 const* vertices, size_t vertex_count) { static_cast<DisplayListPlayer*>(context)->push_device_space_plane_clip({ vertices, vertex_count }); },
            .push_transform = [](void* context, Gfx::AffineTransform const* transform) { static_cast<DisplayListPlayer*>(context)->push_transform(*transform); },
            .push_uncached_clip_path = [](void* context, Gfx::FFI::PathGeometryData const* path, Gfx::WindingRule winding_rule) { static_cast<DisplayListPlayer*>(context)->push_uncached_clip_path(Gfx::PathData::from_ffi(*path), winding_rule); },
            .play_command = [](void* context, DisplayListCommandType command_type, void const* command, PlayedPayload const* payload) { static_cast<DisplayListPlayer*>(context)->play_command(command_type, command, *payload); },
        };
    }
};

void DisplayListPlayer::play_command(DisplayListCommandType command_type, void const* command, PlayedPayload const& payload)
{
    switch (command_type) {
#define PLAY_DISPLAY_LIST_COMMAND(command_type)                            \
    case DisplayListCommandType::command_type:                             \
        play_command(*static_cast<command_type const*>(command), payload); \
        break;
        ENUMERATE_PLAYED_DISPLAY_LIST_COMMANDS(PLAY_DISPLAY_LIST_COMMAND)
#undef PLAY_DISPLAY_LIST_COMMAND
    default:
        // NB: Replay plays only these commands of a page; validation rejects a command of the canvas 2D stream.
        VERIFY_NOT_REACHED();
    }
}

void DisplayListPlayer::begin_drawing_into_surface(NonnullRefPtr<Gfx::PaintingSurface> target_surface)
{
    m_surfaces_drawn_into_before.append(*m_surface);
    m_surface = move(target_surface);
}

NonnullRefPtr<Gfx::PaintingSurface> DisplayListPlayer::end_drawing_into_surface()
{
    NonnullRefPtr<Gfx::PaintingSurface> target_surface = *m_surface;
    m_surface = m_surfaces_drawn_into_before.take_last();
    return target_surface;
}

void DisplayListPlayer::execute_impl(DisplayList const& display_list, FrameInputs const* frame_inputs)
{
    auto const& visual_context_tree = active_visual_context_tree();
    VERIFY(display_list.compatible_visual_context_tree_structural_epoch() == visual_context_tree.structural_epoch());
    VERIFY(m_surface);

    auto callbacks = ReplayCallbacks::for_player(*this);
    if (frame_inputs) {
        VERIFY(frame_inputs->visual_context_tree().rust_handle() == visual_context_tree.rust_handle());
        frame_inputs->replay(display_list, callbacks);
    } else {
        display_list.replay(visual_context_tree, callbacks);
    }
}

}
